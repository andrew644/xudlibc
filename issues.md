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

### 3. (RESOLVED) The UI is not screen-fixed under a 3D camera, and its quads write depth

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

**Resolved.** The variant axis grew a fourth column, `XUD_BATCH_VARIANT_UI`
(`xud_batch.h`), and its pipeline row is the axis's only one with
`depthTestEnable = VK_FALSE` and `depthWriteEnable = VK_FALSE` — the depth
state is now one row per variant column rather than one row for the whole
list (`xud_pipeline.h`/`xud_pipeline.c`). Screen-fixed is the vertex
stage's job: the push row's `flags` member (already written by the frame
loop, already 80 B) carries the draw's variant, and `solid.vert` skips the
per-frame viewProj when the flags name the UI column, whose position is clip
space already (`xud_ui_map_vertex`'s `z = 0.0` and the stage's `w = 1.0`).
The ui rung's fold (`xud_ui_append_draw_list`) now sets the topology to
triangles and moves the batch to the UI column for the frame it appends —
blending stays on, with the same factors as the textured column, because a
glyph over 3D content still needs an alpha over a background that is not
the clear colour. No third pipeline: the one vertex stage branches on the
flags instead, which is the same fact the pipeline row's depth state states
from the other side.
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

### 5. (RESOLVED) No background / clear colour control

The clear colour is hard-coded to (0.02, 0.02, 0.05) in `xud_renderpass.h:83-86`
and baked into the render pass's clear values. There is no
`ClearBackground`/`SetClearColor` equivalent in `xud.h`, so every app shares the
library's blue-grey and cannot change it without touching the pass.

**Resolved.** The colour is now the renderer's state rather than a pass constant:
the public `xud_clear_background()` (raylib's `ClearBackground`) stores the
caller's `XudColor` in the renderer, and the renderer's draw entry writes it into
the render pass's clear value - the colour row alone, through the new pure rung
`xud_renderpass_set_clear_color()` - before the frame loop begins the pass it
records, so a call in frame N changes frame N's own clear. A frame drawn before
any call is cleared with the old default: the `XUD_RENDERPASS_CLEAR_COLOR_*`
macros are now the pass's default, quantized to the byte grid at renderer open.
See `xud.h` (drawing section) and `xud_core.c`'s note above the setter.

### 6. (RESOLVED) No 2D drawing surface and no per-draw transforms

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

**Resolved.** The three gaps are one column and one channel. The screen-space
surface is the pipeline list's UI column (`XUD_BATCH_VARIANT_UI`, depth off,
screen-fixed through the vertex stage's flags branch, the same row issue 3's
UI folds into), and the two public draws that name it are
`xud_draw_texture()` (a textured screen-space quad: the texture's own extent in
pixels at the caller's position, through the shapes rung's `xud_draw_texture`
append) and `xud_draw_text_screen()` (the font's pen walk at a pixel position,
the 3D walk's arithmetic with the position's pixels mapped by the same
mapper). Both refuse a context with no live swapchain extent and a batch that
does not fit, and both record their quads into the UI column rather than the
textured one, so a frame holds a 3D string and a screen string at once.

The per-draw transform is the row's own push channel: `XudBatchDraw` carries
a `modelMatrix` and a `tint` (identity and white for every row a plain append
records), the frame loop's push row copies the drawn row's pair into the
push-constant row it already writes beside every draw, and
`xud_draw_mesh_ex()` / `xud_draw_model_ex()` are the two public draws that name
a caller's matrix and colour rather than the renderer's per-frame state - the
plain forms are unchanged, and a posed model stamps every row its primitives
record with the same matrix rather than one per mesh.

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

### 8. (RESOLVED) Smaller parity gaps

- No `SetTextureFilter`/`SetTextureWrap` equivalent — filter and wrap are fixed
  at `xud_create_texture` and cannot be changed on a live texture (no
  `vkDestroySampler`/recreate path exists in `xud_texture.c`).
- No vsync or target-FPS control — `glfwSwapInterval` is never called, so every
  app runs at the GLFW default (vsync 1) with no way to change it.
- Audio has no volume or pitch surface (both are on the `todo.md` path; the
  sound node is created with `MA_SOUND_FLAG_NO_PITCH` and no mixer node).

**Resolved.** All three gaps are closed at the public surface, with the driver
rung owning the state each setter moves:

- `xud_set_texture_filter()` / `xud_set_texture_wrap()` take one token per axis
  and refuse a token outside its axis as `XUD_INVALID_ARG` before anything is
  touched. The change is a recreate of the texture's own sampler —
  `vkDestroySampler` and `vkCreateSampler` from the record's own create info
  with the one token moved — the record's `samplerInfo` rows are updated before
  the recreate and restored on a refusal, so the record and the live sampler
  name one state on every answer, and a token the sampler already names is the
  no-op answer checked off those same rows.
- `xud_set_target_fps()` / `xud_get_target_fps()` map fps to the window's
  present interval the way raylib's own `SetTargetFPS` does: 0 asks for the
  unsynchronised interval and any positive number its synchronised one. The fps
  is recorded on the window's own member before the interval is asked, so the
  getter answers the caller's ask rather than the interval the window system
  cannot read back — the same split raylib's `GetTargetFPS` makes with its own
  recorded default.
- `xud_set_audio_volume()` / `xud_get_audio_volume()` and
  `xud_set_audio_pitch()` / `xud_get_audio_pitch()` keep the value in the
  stream's record, which is where a setter that runs before the stream's first
  play parks it: the node the first play builds takes the record's value, and a
  setter on a stream that is playing writes it on the row's own node — the node
  between the row's sound and the endpoint the level rides on, miniaudio's
  per-node write, the same one raylib's `SetAudioBufferVolume` runs. Volume is
  refused outside [0.0, 1.0] (0.0 the silence, 1.0 the block's own peak —
  raylib's "1.0 is max level") rather than clamped, and pitch is refused at or
  below zero, the one the engine would take and do nothing with, which would
  leave the record and a live row naming different pitches. The row's
  `MA_SOUND_FLAG_NO_PITCH` is dropped so the pitch write is the one it takes.

## Performance

### 9. (RESOLVED) The frame loop is synchronous single-flight

`xud_frame_draw` waits on the frame fence with an infinite timeout
(`xud_frame.c:1144`) after every present, so the CPU never starts frame N+1
while the GPU is still drawing frame N. With a two-image swapchain the GPU is
always one frame behind the CPU and the two never overlap. This is correct,
and it is what makes the rest of the loop (one command buffer, one acquire
semaphore, per-image fences) simple, but it caps throughput at
max(CPU frame, GPU frame) and it is a structural decision worth recording:
moving to triple-buffering with per-frame semaphores is a rewrite of the frame
rung, not a patch.

**Resolved — by decision, recorded in the code.** The decision lives in
`xud_frame.c`'s own header: the fence wait at step 7 runs to completion
before `xud_frame_draw` returns, so the CPU cannot start frame N+1 while the
GPU is still drawing frame N, and throughput is capped at max(CPU frame, GPU
frame) rather than their overlap. That is the price the fence being one per
image, the acquire/submit/present triple carrying no frame array, and a
completed frame leaving the device idle when it returns all buy. Moving to
triple-buffering with per-frame semaphores is a rewrite of the frame rung,
not a patch; until that rewrite the wait is a recorded ceiling rather than a
defect to fix.

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

### 11. (RESOLVED) Every texture close stalls the whole device

`xud_texture_close` and `xud_texture_close_all` both run
`xud_device_wait_idle` (`xud_texture.c:1197, 1261`), and the allocator close
waits again. Closing N textures therefore takes N full device idles in series.
A per-texture fence (the texture's own upload fence is already the model for
this) would let closes proceed without idling in-flight frames from other
objects.

**Resolved — by decision, recorded here.** The N-stall cost does not
materialize in this loop, and a per-texture fence cannot express what the
close needs. A texture's own upload fence is waited at creation, so at
close nothing of the texture rung's is in flight; the only in-flight reader
a live texture can have is the frame loop's submit — and the frame loop owns
that fence and waits it before `xud_end_drawing` returns (`xud_frame.c`,
step 7: wait, reset, `lastFrameFence = NULL`), so on the normal path every
close-time `xud_device_wait_idle` answers immediately: N closes are N no-ops,
not N stalls. The guard is active only on the path where a previous frame
failed partway after its submit — the one state where a submission really is
in flight and `VUID-vkDestroyImage-image-00009` (all uses completed) demands
the wait. Vulkan has no fence meaning "all uses of image X are complete";
the only fence that covers a texture's sampling is the frame loop's, and
reaching up into it from the texture rung would invert the rung order — the
same frame-rung rewrite issue 9 names for single-flight. The rationale lives
in `xud_texture.c` at both close sites, and the guard is the uniform one
every destroy rung that a frame may still be reading uses (mesh, descriptor,
render pass, swapchain, allocator).

### 12. (RESOLVED) Input is one frame behind by design

`xud_input_poll` runs at the end of `xud_end_drawing`, after the present
(`xud_core.c`). This matches raylib's `EndDrawing` ordering and is documented,
but the consequence is worth stating: window events (WM close, resize) and key
presses only become visible on the frame *after* the one they happened in, and
`xud_begin_drawing` itself polls nothing. If issue 4 is fixed by polling a
window-close flag, that flag will also lag by a frame.

**Resolved — by decision, recorded in the code.** The consequence is stated
where the API states its own contract: the window section of `xud.h` records
that the state a query answers about is the state as of the poll that opened
the current frame, that nothing polls on demand, and that the window
manager's close flag and resize events are read by the SAME poll — so
`xud_window_should_close()` (issue 4's fix) answers the close state as of the
poll that ended the LAST frame, and a window closed or minimized mid-frame is
seen on the next one. That is raylib's `WindowShouldClose`'s own lag for its
exit key, a property of the poll living at the frame boundary rather than a
defect: the boundary is the one place two queries in one frame read the same
records. The same rule is restated at the poll itself (`xud_input.h`), at the
frame boundary that owns it (`xud_core.c`), and at the close query
(`xud_window.h`).

## Inconsistencies and minor issues

### 13. (RESOLVED) Two different window-extent bounds

The public `xud_init_context` refuses extents above `XUD_MAX_WINDOW_EXTENT`
= 16384 (`xud.h:114`), while the window and swapchain rungs accept up to
`XUD_WINDOW_MAX_EXTENT` = INT_MAX (`xud_window.h:104`). The header comment calls
16384 a "host-side sanity bound ... until the real gate exists" (the device's
`maxImageExtent2D` is recorded in the caps but never consulted), so today a
window the machine can present (many drivers allow 32768+) is refused as
`XUD_INVALID_ARG` by the public entry. Either wire the caps value into the
check or state the 16384 cap publicly.

**Resolved.** The caps value is wired into the check: the pure gate
`xud_window_extent_ok()` answers true when an extent sits inside both
`XUD_MAX_WINDOW_EXTENT` and the device's `maxImageDimension2D`, so the
effective bound is the smaller of the two — the machine's limit only ever
tightens the public one, never loosens it. Both public entry points run it:
`xud_init_context` answers `XUD_UNSUPPORTED` for an extent the machine
cannot present (after the bring-up has the caps to read), and
`xud_set_window_size` does the same before it would touch the window, so a
resize the machine cannot present is refused rather than failing at the
frame boundary that would recreate the swapchain. The public bound still
answers `XUD_INVALID_ARG` on its own, above or without a device.

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

### 17. (RESOLVED) A batch that hits its growth bound refuses forever

`xud_batch_grow` answers `XUD_UNSUPPORTED` at the bound, `xud_begin_drawing`
breaks out of the grow loop and keeps the ask set, and every subsequent append
is refused (`xud_shapes_note_refused`). The frame still draws the content that
fits, but the app has no public query to ask *why* appends are failing or how
much capacity it is short by — the only way out is to shrink the scene. A
`xud_get_batch_stats`-style public query (the internal `xud_batch_stats`
already exists) would make the state diagnosable.

**Resolved.** `xud_get_batch_stats(XudBatchStats*, XudContext*)` now answers
above the batch rung from `xud_core.c`, reading the batch's own rows through
its accessors rather than its fields: the live vertex/index counts, both
capacities (a new `xud_batch_index_capacity` accessor reads back the 3n/2 the
layout installed rather than re-deriving it), the recorded draw rows, the
arena's total, and the refused ask. The two flags are the diagnosis:
`growthAsked` is the batch's own growth question the frame boundary asks
before it grows, and `atBound` (the batch's next capacity is 0 — the grow
cannot happen) is the state in which an ask above the capacity is refused
until the scene fits the capacity the batch has. The headless half covers the
guards, the clean diagnosis, the refused-append half and the at-bound half
against a shell context; the GPU half reads the same query back through a
completed context.

## Correctness found after the first read

### 18. (RESOLVED) A stream plays at the device's rate rather than its own

`xud_sound_play` builds the row's `ma_audio_buffer_ref` over the decoded
block and never sets the ref's `sampleRate`, and `ma_audio_buffer_ref_init`
leaves it zero (miniaudio's own `ma_audio_buffer_init` works around the same
gap by setting `ref.sampleRate` from its config). The engine node reads the
data source's rate to work out its resampler ratio, so the zero rate is a
degenerate ratio and the walk is timed at the device's rate rather than the
stream's: on a 44100 Hz device the two-second, 22050 Hz take ended in about a
second, twice the speed it was recorded at.

**Resolved.** `xud_sound_play` sets `row->bufferRef.sampleRate` from the
record's rate after the ref's init, the same line miniaudio's own buffer init
sets. The device half of `xud_test_audio.c` pins it with a duration check on
the two-second take: the public question is polled with a clock, and the end
must land within half a second of the stream's own length, which is the window
a walk timed at the device's rate misses.

