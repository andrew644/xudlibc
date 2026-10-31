# xudlibc — TODO

Progress tracker. Move items between sections as work happens and add a dated one-liner.
Finished items and their long notes move to `todo_completed.md` (newest first) — never delete a finished
step, just move it, so the active tracker stays short and the log stays complete.

## Now

- 2026-10-31 note: **`examples/breakout_3d.c` grew the input and the clock rungs it had been
    standing in for** — the run now ends on the player holding Escape, and the game is played with
    the left/right arrows (A/D too) rather than only watched. Input comes from GLFW directly, which
    is the temporary half asked for until xud has an Input wrapper: the example includes `xud_core.h`
    — its ONE exception to "user code includes xud.h only" — to reach `context->window.glfwWindow`,
    the GLFW window the CONTEXT already owns, and calls `glfwPollEvents()` + `glfwGetKey()` on it. It
    creates no window of its own (a second GLFW window would be a second surface in the same session
    and the game would be played on a window nothing draws to), and `xud_window.h` is what brings
    `<GLFW/glfw3.h>` AFTER `<vulkan/vulkan.h>`, so glfw3.h's own include-order trap (its Vulkan
    declarations sit inside `#if defined(VK_VERSION_1_0)`) is satisfied by the ladder rather than by
    the example naming GLFW's include dir. `glfwPollEvents()` is load-bearing: GLFW's Wayland backend
    receives key events through an epoll loop that only its poll/wait forms drain (wl_window.c 2938
    → handleEvents) and nothing else in this tree polls GLFW. The paddle is the player's while a key
    is held and the game's own autopilot while none is — that coexistence is what keeps an unattended
    run a game rather than a ball that always falls out. The same rung brings GLFW's timer: `glfwGet
    Time()` paces the game at 60 steps a second in WALL time, and it has to because GLFW's window runs
    as fast as it can — measured here at about 3250 frames a second, so one step per frame was a ball
    crossing the field fifty times a second, a blur a hand cannot play and cannot time an Escape
    against. The fallback end of a run is therefore 60 seconds of wall time (`-DXUD_BREAKOUT_MAX
    _SECONDS=N` overrides) rather than a frame count, and clearing the field re-ARMS it for a new round
    rather than ending the run, because the run's end is the player's now. This supersedes the "No input
    and no clock" sentence in the breakout note below. Verified here: the paced run does 298 steps in
    5.0 s (60/s) over 16235 frames (~3250 fps); the escape path — forced at step 200 in a scratch copy
    outside the tree, since no key event can be sent to a real window from here — prints "ended by
    escape", closes with `XUD_OK` and exits 0, and the time-cap path does the same at the cap, both
    validation-clean (36 stderr lines, INFO only, no ERROR, no WARNING, no VUID); the unattended run
    stays open past the old 900-frame end. NOT exercised on this machine: an actual key press. niri is
    the compositor (`XDG_CURRENT_DESKTOP=niri`) and it advertises `wl_seat`/`wl_keyboard`, which is
    what GLFW's Wayland backend binds (wl_window.c 2091 `wl_seat_get_keyboard`, and GLFW 3.5.1 has no
    focus-based modern input), so the path exists — the press itself is the user's to test. Suite
    unchanged: 2574 headless, 3309 GPU, 2574 under sanitizers, all green.

- 2026-10-31 note: **`examples/breakout_3d.c` landed** — the first example that is a game rather than a
    drawing: a ball, a paddle and a field of 6 bricks at three depth layers, and the collisions that
    bounce the ball off the walls, off a brick's shallowest-overlapping axis and off the paddle's top
    plane. What makes the game 3D is the example's own pinhole, and that is the one thing a reader
    should understand before changing it: there is no camera + transform step, so a shape's coordinates
    are clip-space coordinates and `xud_breakout_project()` does the divide by the distance from the eye
    THERE rather than in the shader (the shader's `w` is 1). That divide is exact because every quad the
    example draws lies in a plane of CONSTANT z — four corners sharing a depth share the projection's
    scale and need no perspective correction — and it is also why `xud_draw_cube()` cannot be used here
    at all: a projected box's corners are not centre±half in the space it is handed, and a box projected
    by hand would be 36 vertices of the batch's 64. So ONE public shape call is used and only one.
    The geometry is stated in INTEGER milli-world-units with the float macros derived from them for one
    reason: the fit checks the game needs (the box inside the frustum at its NEAREST depth, the field
    inside the box, a brick inside its span, two bricks too far apart in depth for the ball to be inside
    both) are `static_assert`s, and a float `static_assert` is a `-Wpedantic` ERROR here, not a warning.
    The frame budget is the example's own count of its own quads (9 quads = 54 vertices and 54 indices)
    against the batch's capacity today restated (64/96), because `xud_batch.h` is internal and user code
    includes `xud.h` only. No input and no clock: the paddle chases the ball at a bounded speed BELOW the
    ball's fastest x so the game sometimes misses, the bottom of the box is open so a miss is a miss
    rather than a bounce, and a miss resets the ball rather than ending the run — the game is
    deterministic, so two runs are the same run. Verified on this machine: the run COMPLETES in 583
    frames, clears all 6 bricks, takes 1 miss, `xud_close_context` answers `XUD_OK` and exit is 0, with
    the 36 stderr lines being the loader's and the validation layer's INFO only — no ERROR, no WARNING,
    no VUID. The projection's own arithmetic was probed separately (a pure-math program, no driver): every
    quad the game can draw lands inside NDC on all three axes and non-degenerate, and the depth ordering
    is the one the scene claims (back plane z +0.400 = far, bricks +0.200 to -0.400, paddle -0.600 = near,
    the same brick 0.112 NDC wide at the far layer and 0.142 at the near one). The headless suite is 2574
    checks green and the GPU run 3309, both unchanged by this step. The "Examples set + README" item below
    stays open — one example is not a set.

- 2026-10-31 note: **the sphere landed as S25** — see the *Steps complete (S25 …)* section at the top of
    `todo_completed.md`. What landed there: `xud_shapes_append_sphere()` and the public `xud_draw_sphere()` above it in
    the same `xud_shapes.h/.c` rung, the sphere as a **closed shared-vertex mesh** (17 vertices, 30 triangles, 90 indic
es —
    two single-vertex poles plus three rows of five, two fans of five triangles and two bands of five quads, the longit
ude
    wrapping so there is no seam column and no row of coincident poles), the normal being the **unit direction that pla
ced
    the vertex** (a sphere's normal is radial by definition, which is the reason it shares vertices where a box cannot,
    and the reason no shader reads a flat constant any more), the tessellation being a claim about the **batch's** capa
city
    rather than about the shape (this rung is pure and cannot grow the batch, so the default is the finest that fits th
e
    capacity a context opens at), and — the fact this step is really about — that a sphere spends **90 indices over 17
    vertices**, above the batch's 3n/2 per-vertex headroom, so the **index** bound is the one that refuses a sphere and
    the vertex bound has 47 slots to spare: the exact opposite of the triangle's and the box's, and the closed-mesh bou
nd's
    counterexample. A closed manifold triangulated surface has F = 2V-4 faces by Euler (3F = 2E and V-E+F = 2), so it
    needs 3F = **6V-12** indices - four times that headroom - and the sphere's 90 over its 17 vertices is exactly 6V-12
    (xud_shapes.h asserts it); the box's 1.5 per vertex is its SPLIT-per-face spelling (six disconnected quads, 4 verti
    ces and 6 indices each) rather than that bound. **xud_batch.h's index-capacity note is corrected here rather than le
    ft
    to read as a bound** - it said "a closed triangle mesh needs at most 3n/2", which is the split-per-face spend wearin
    g
    the word "bound"; the macro itself, the two asserts that size the staging arena against the allocator's blocks and
    the grow path (which doubles BOTH, so the ratio is kept rather than corrected) are untouched: a batch asked for mor
    e
    shared-vertex geometry than its headroom holds is still refused rather than resized, and a caller who wants a fine
    r
    shared-vertex mesh asks for a bigger capacity. The tests area check
s the
    mesh rather than a table — every position one radius from the centre, every normal unit **and** radial (the two hal
ves of
    that claim fail separately, and a normal copied from the flat +Z constant fails both), every index naming a vertex
    that exists and every vertex named by at least one, every triangle non-degenerate and wound outward — against one n
amed
    tolerance rather than exact equality, because a sphere's positions come from `sinf`/`cosf`, which are rounding, unl
ike the
    box's dyadic extents. The GPU half's last frame is the sphere's rather than the box's: 6 + 90 indices is **exactl
y** the
    initial capacity's 96-slot headroom, so a sphere and a box cannot share a frame at that capacity and the box is the
refused
    through a real context with the counters checked unchanged. Also fixed here: `xud.h` carried a **duplicate** `xud_
draw_t
    riangle_3d` declaration under `xud_draw_cube()` (legal C, a repeated prototype, so nothing in the tree noticed it) —
    it is replaced by the sphere's declaration, which is what makes the header's shape count three rather than two with
    one of them named twice.
- 2026-10-31 note: the `-fsanitizer` build still answers **72 bytes leaked in 3 allocations (32+32+8)** at exit, uncha
nged
    from S24's note above — the three are the validation layer's pair around `vkCreateDevice` and GLFW's libdecor, all
    inside the dlopen'd driver, and nothing this step adds allocates. With `ASAN_OPTIONS=detect_leaks=0` the same build
    runs the full headless suite green (2474 checks, 0 failures, no ASan/UBSan report).

- 2026-10-27 note: **the box landed as S24** — see the *Steps complete (S24 …)* section at the top of
    `todo_completed.md`. What landed there: `xud_shapes_append_cube()` and the public `xud_draw_cube()` above it in the
    same `xud_shapes.h/.c` rung, the box's six faces as **data** (a normal table and a corner-sign table, 6 rows of 4
    corners of -1/+1, so a box's 24 corners are the 8 corners of one box rather than 12 points inside it), the one cont
    ext guard the public draw calls now share, and `tests/xud_test_shapes.c` gaining the box's **geometry** checks — t
    he positions read off the arena and checked to sit on their own face's plane and inside the other two extents, the
    normals checked to be unit axis vectors, and both triangles of all six faces checked to have cross products along t
    hat face's OUTWARD normal, which is what would catch a face table written with the wrong winding or the wrong corne
    r signs. A box is 24 vertices and 36 indices, exactly the batch's 3n/2 index headroom for 24 vertic
    es — the closed-mesh case of that bound — and **both** capacities are checked rather than only the vertex one,
    because an index names a live vertex rather than a new one and a batch whose existing content reuses vertic
    es can be full on its indices while it still has twenty-four vertices' worth of room.
- 2026-10-27 note: the `-fsanitizer` build answers **72 bytes leaked in 3 allocations (32+32+8)** at exit, and the all
    three are **inside the dlopen'd driver**, not ours: the frames under `malloc` are `<unknown module>` and the first
    xud frame is the `vkCreateDevice` call in `xud_device_open`. The count is identical with `VK_INSTANCE_LAYERS=` set
    empty (so not the validation layer's), identical on the tree as it stood before S23/S24 (probed with `git stash`),
    and unchanged by either step — so the library's own rungs leak nothing today and the sanitizer build's leak report
    is a driver fact to expect, not a regression to chase. With `ASAN_OPTIONS=detect_leaks=0` the same build runs the
    full headless suite green (2433 checks, 0 failures, no ASan/UBSan report).
- 2026-10-27 note: `examples/hello_triangle.c` was **rewritten as the 46-line simple program** — no flag parsing, no
    clock, no usage text: one context, a `XUD_HELLO_FRAMES`-frame loop, one triangle, one teardown, every call's answer
    printed and nothing ignored. The `--hold`/`--frames`/`--help` command line it carried is gone (the frame count is
    what keeps the window open now); the example is `xud.h` only and still names no driver object.
- 2026-10-27 note: the frame loop's one `vkCmdDrawIndexed` was **drawing zero instances** — the PFN's order is
    `(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance)`, so the `0u` in the third slot was a legal
    draw of no instances, which is a clean validation run, a correct clear and nothing on screen. It is `1u` now, and
    the comment beside it says the argument order rather than claiming "0 is the draw-once form".

- 2026-10-26 note: the **shapes rung and the public drawing API landed as S23** — see the *Steps complete (S23 …)* section
    at the top of `todo_completed.md`. What landed there: `src/xud/xud_shapes.h/.c` (a new rung between the batch and the
    public API, `xud_shapes_append_triangle()` appending three vertices and three indices **atomically** into the batch's
    four streams and its index buffer), `xud.h` gaining the two value types a caller passes down (`XudVector3`/`XudColor`,
    raylib's own), the public frame pair `xud_begin_drawing()`/`xud_end_drawing()` and `xud_draw_triangle_3d()`, `xud_cor
    e.c` gaining the frame pair above the context (neither half calls a driver function: the opening half is the batch's
    reset and the closing half is the renderer's one frame call plus `xud_renderer_resize()`, the reader of the recreate
    ask), `examples/hello_triangle.c` (the first program that draws anything, `xud.h` only) and `tests/xud_test_shapes.c`
    (a new area, above the batch and below the core). What is **still** open above it is the rest of raylib's 3D shape f
    -amily and the camera + transform math — which is why a shape's coordinates are **clip-space** coordinates today: th
    e frame's `viewProj` and this draw's model matrix are both identity, so the vertex shader answers with the position i
    t read out of the POS stream unchanged. A public call that names a recreate is still an ask, and nothing about the pu
    blic API surface changed in S22 — it changed here.

- 2026-10-26 note: the **resize / recreate step landed as S22** — see the *Steps complete (S22 …)* section at the top of
    `todo_completed.md`. What landed there: `xud_window_resize()` / `xud_window_framebuffer_size()` (GLFW's set/get pair,
    and the **framebuffer** extent is what a swapchain is created at, never the window's requested one), `xud_renderpass_r
    ecreate_depth()` (the depth attachment is recreated because a framebuffer attachment owes the framebuffer's own extent)
    and `xud_renderer_resize()` (the reader of `XudFrame.recreateWanted`, running between draws in the one order that is c
    orrect), plus the wait-idle guard `xud_swapchain_recreate()` now owes the `oldSwapchain` retirement. Nothing about the
    public API surface changed — the resize is rung-level, and a public call that names a recreate is still an ask.

- [ ] Batch **consumers** (last part): the handle layer's material **record** — the batch's `materialIndex` names a
    material slot and `xud.h` still declares no create/close function that takes a handle
    - 2026-10-26 note: the **transparent variant landed as S20** — see the *Steps complete (S20 …)* section at the top
      of `todo_completed.md`. What landed there: the pipeline rung owns a **pipeline per row of the batch's variant list**
      (`pipeline[XUD_BATCH_VARIANT_COUNT]`, one layout for the whole list — the D1 below), the blend attachment is a
      **per-row table** (opaque `blendEnable = VK_FALSE`, transparent `VK_TRUE` + `VK_BLEND_FACTOR_SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA`
      + `VK_BLEND_OP_ADD`, the Vulkan form of raylib's default `BLEND_ALPHA`), the frame loop **binds the row a draw's
      variant names** beside that draw rather than once per frame, and the GPU test's last frame runs the transparent
      state end to end. The sorted-draw rule is the **D2** below — caller append order, the library never sorts, no
      sort key on the row — and it needed no code because the row array already **is** the append order.
    - 2026-10-26 note: the **third of this item landed as S19** — see the *Steps complete (S19 …)* section at the top
      of `todo_completed.md`. What landed there: **`vkCmdPushConstants`** in the frame loop, recorded beside every draw
      row the batch recorded rather than once per frame (against the layout's **own recorded** range — `descriptor->push
      Range.stageFlags/offset/size` — not constants of the loop's, which is the same discipline the set-0 bind follows
      with `pipeline->layout`), **`xud_frame_fill_push_row()`** building the `XudPushRow` from the batch's draw row (the
      model matrix identity, the batch's `XUD_BATCH_NO_MATERIAL` sentinel mapped to slot 0 because the push member is an
      index a read could use, texture 0 because set 1 is never allocated, flags the variant the draw was flushed under),
      and `shaders/solid.vert` placing the vertex by **`viewProj * modelMatrix`** — so both channels of the layout are
      live end to end now. What is still open, itemised against this item's list:
      - **The handle layer's `XudHandleSlot.record`**: still the NULL sentinel, and `xud.h` still declares no create/clos
        e function that takes a handle. What the batch's `materialIndex` would name is a material slot's record, and what
        such a record holds (a pipeline-variant index + the set-1 bindings) is an open design decision — see *Ask before*
        in `AGENTS.md` and the Blocked section below.
      - **The transient ring**: `xud_allocator_ring_acquire`/`xud_allocator_ring_reset` still have no caller outside the
        allocator area's own test. Neither the batch's arena (S16) nor the block's arena (S18) wanted it — a ring carve is
        bytes a later lap overwrites mid-frame, and both arenas' bytes have to survive from a host write to a transfer read.
      - 2026-10-26 note: the **second half of this item landed as S18** — see the *Steps complete (S18 …)* section, below
        S19, in `todo_completed.md`. What landed there, itemised against this item's list: the **per-frame uniform copy** (the
        `viewProj` row `xud_descriptor_fill_uniform_block()` writes now has a host-visible staging arena of its own, is
        written every frame by `xud_descriptor_write_block()` and is copied into the GPU buffer every frame by the
        descriptor rung's ONE `vkCmdCopyBuffer` call — `xud_descriptor_upload()` — and the frame loop calls both), **`vkCmdBindDescriptorSets`
        for set 0** (one call, against the pipeline's own layout, `firstSet` 0, count 1), and the **shader reading the set-0
        `viewProj`** (`shaders/solid.vert` declares the `XudFrameBlock` and places the vertex by it). What was still open at
        that step, and what S19 above closes the first of: `vkCmdPushConstants` and the push-constant model matrix, the
        transient ring, the transparent variant and the handle layer's `record`.
      - 2026-10-26 note: the **first half of this item landed as S17** — see the *Steps complete (S17 The batch's consumers
        …)* section, below S18, in `todo_completed.md`. What landed there: the pipeline's vertex input over the batch's four
        streams, `vkCmdBindVertexBuffers`/`vkCmdBindIndexBuffer`, the indexed multi-draw replay, `xud_batch_upload()` from
        the frame loop and the per-frame batch reset. `vkCmdBindDescriptorSets` and `vkCmdPushConstants` were still nowhere
        in `src/xud/` at that step.
      - 2026-10-26 note: the **batch rung itself landed as S16** — see the *Steps complete (S16 …)* section below S17 in
        `todo_completed.md`.
      - 2026-10-26 note: the **first half of this item landed as S17** — see the *Steps complete (S17 The batch's consumers
        …)* section, below S18, in `todo_completed.md`. What landed there: the pipeline's vertex input over the batch's four
        streams, `vkCmdBindVertexBuffers`/`vkCmdBindIndexBuffer`, the indexed multi-draw replay, `xud_batch_upload()` from
        the frame loop and the per-frame batch reset. `vkCmdBindDescriptorSets` and `vkCmdPushConstants` were still nowhere
        in `src/xud/` at that step.
      - 2026-10-26 note: the **batch rung itself landed as S16** — see the *Steps complete (S16 …)* section below S17 in
        `todo_completed.md`.
      - **The pipeline's vertex input**: `xud_pipeline_fill_vertex_input()` overwrites the zero row `xud_pipeline_fill_state()`
        writes with the batch's four binding + four attribute rows, pointing at the **batch's own arrays** (`XUD_BATCH_STREAM_COUNT`
        of each), and `xud_pipeline_open()` takes the batch for that reason — the renderer opens the batch before the pipeline
        and closes it after, which is what makes that pointer legal for the pipeline's whole life.
      - **Two of the three command-buffer bindings**: `vkCmdBindVertexBuffers` (one call, whole array, `firstBinding` 0,
        offsets 0 because each stream is its own `VkBuffer`) and `vkCmdBindIndexBuffer` (offset 0, `VK_INDEX_TYPE_UINT32`).
        `vkCmdBindDescriptorSets` and `vkCmdPushConstants` are still nowhere in `src/xud/`.
      - **The draw**: `vkCmdDrawIndexed` per recorded row of the batch, `vkCmdDraw` and `XUD_FRAME_DRAW_VERTEX_COUNT`/
        `XUD_FRAME_DRAW_INSTANCE_COUNT` gone from `xud_frame.h`/`.c`. **Note for the next step**: this machine's `vulkan-headers`
        1.4.363 declares `vkCmdDrawIndexed` **without** an index-buffer argument — `(commandBuffer, indexCount, instanceCount,
        firstIndex, vertexOffset /* int32_t */, firstInstance)` — and declares `vkCmdBindIndexBuffer` beside it, so the index
        buffer is bound as state and the draw reads it.
      - **The upload**: `xud_batch_upload()` is called from `xud_frame_draw()` behind a `HOST_WRITE → TRANSFER_READ` barrier
        (the arena is mapped once for the batch's life, so the barrier is what makes the appends visible, not a map/unmap pair)
        and a `TRANSFER_WRITE → VERTEX_ATTRIBUTE_READ` barrier, over the **live** byte counts.
      - **The frame loop's shape**: the batch is flushed **before** the command buffer is begun (`xud_batch_record_draw()` +
        a stats re-read, both pure host work) and `xud_batch_reset()` runs at the end of a frame that completed, so the next
        frame appends into an empty batch and the arena's bytes are rewritten in place, never appended to. The multi-draw
        replay is in place; nothing flushes **mid**-frame yet, so `xud_batch_should_flush()` is still a decision no caller acts
        on and the pending run is one row per frame.
      - **The shader**: `shaders/solid.vert` reads the four streams at locations 0..3 and outputs `outUv`/`outColor`/`outNormal`
        at 0/1/2; `shaders/solid.frag` reads `inColor` at **1**, which is where the vertex stage outputs it (the validation
        layer's `VUID-RuntimeSpirv-OpEntryPoint-08743` refuses a frag input whose Location/Component slots the previous stage
        does not output, and it fired on this machine — in the **headless** run too, because the ungated
        `xud_test_init_bringup_boundary` completes a real context here). `gl_Position = vec4(inPos, 1.0f)`: the push-constant
        model matrix and the set-0 `viewProj` are still unread, so the pipeline layout's push range and two set layouts stay
        declared-but-unread, which is legal rather than half-built.
      - **Still open, unchanged**: the per-frame uniform update (`xud_descriptor_fill_uniform_block()` writes an identity
        `viewProj` and nothing ever touches that row; the copy that puts the block's bytes into the GPU buffer is the frame
        loop's and is not in the tree — and it is what the transient **ring** will want, which `xud_allocator_ring_acquire`/
        `xud_allocator_ring_reset` still have no caller for), the transparent variant (`XUD_BATCH_VARIANT_TRANSPARENT` is
        declared and the flush decision keys on the variant index, but only the opaque pipeline exists — `blendEnable = VK_FALSE`),
        and the handle layer's `XudHandleSlot.record` (still the NULL sentinel, `xud.h` still declares no create/close function
        that takes a handle).
      - 2026-10-26 note: the **batch rung itself landed as S16** — see the *Steps complete (S16 …)* section below S17 in
        `todo_completed.md`.


## Next

- [ ] Shapes module, the rest of it (`DrawLine3D`, `DrawPoint3D` and the wire forms, `DrawMesh` — 3D only,
    there is no 2D shape family in the API)
    - 2026-10-31 note: **the wire forms of this item landed as S27** — `xud_draw_cube_wires()`/`xud_draw_sphere_wires()`
      and the `xud_shapes_append_cube_wires()`/`xud_shapes_append_sphere_wires()` halves they share with the line, in the
      same `xud_shapes.h/.c` rung, on the topology axis S26 grew rather than on a new one: a wire is the `LINE_LIST` row
      over another shape's edge set, which is raylib's `DrawCubeWires`/`DrawSphereWires` read literally (the same box,
      a different `rlBegin` mode). The two edge sets are the facts the step is about: the wire box is the **8 corners of
      one box and the 12 edges of the cube graph** (E = 3V/2, the cube's V-E+F = 2 with F = 6) rather than the filled
      box's 24 corners-of-six-faces — a wire has no face, so there is no normal a shared corner would contradict — and
      24 indices over 8 vertices is **3 per vertex, twice the filled box's 1.5**, the third side of the batch's 3n/2
      headroom being a choice; the wire sphere is the **globe's grid, not the mesh's wireframe** — the same 17 vertices
      the filled sphere places in the same order, joined by 15 latitude edges and 20 meridian edges = 35 edges and 70
      indices, **BELOW** the filled one's 90, because a band's diagonal is an artefact of triangulating a quad rather
      than an edge of a sphere. What is still open of this item: `POINT_LIST` (a row of the list S26 grew, not a row it
      needed) and `DrawMesh`, which is the mesh / vertex-buffer rung rather than a shape of this rung's.
    - 2026-10-31 note: **the line and point forms of this item landed as S26** — `xud_draw_line_3d()`/`xud_draw_point_3d()`
      and the `xud_shapes_append_line()` half they share with the triangle, the box and the sphere, in the same
      `xud_shapes.h/.c` rung, plus the **topology axis** they needed: the pipeline rung's variant list became a
      **(topology × variant)** list (`XUD_PIPELINE_ROW(topology, variant)`, 4 rows today, one layout for the whole list),
      the batch's draw row gained `topology` beside `variant`, the frame loop binds the row the pair names, and
      `xud_batch_set_topology()` closes a run into another row of the SAME batch rather than flushing — raylib's
      `rlBegin(mode)` shape (rlgl.h 1454-1478), because topology is `VkPipelineInputAssemblyStateCreateInfo::topology`, i.e.
      pipeline state, not a draw-call argument. A point is a short line, not a `POINT_LIST` row: raylib's own `DrawPoint3D`
      is "actually a small line" (raylib.h 1586) and is built with `rlBegin(RL_LINES)`. What is still open of this item,
      itemised: the **wire forms** (`xud_draw_cube_wires()`/`xud_draw_sphere_wires()`, the line topology over the box's and
      the sphere's edge sets — the topology exists now, the edge tables do not), `POINT_LIST` (a row of the list S26 grew,
      not a row it needed), and `DrawMesh`, which is the mesh / vertex-buffer rung rather than a shape of this rung's.
    - 2026-10-31 note: **a third shape of this item landed as S25** — `xud_draw_sphere()` and the `xud_shapes_append_sph
      ere()` half it shares with the triangle and the box, in the same `xud_shapes.h/.c` rung; see the *Steps complete
      (S25 …)* section at the top of `todo_completed.md`. What the rest of this item owes: the line/point and wire form
      s, which need `LINE_LIST`/`POINT_LIST` topologies and so a row of the pipeline list rather than the pipeline's on
      e `TRIANGLE_LIST` row, and `DrawMesh`, which is the mesh / vertex-buffer abstraction in the *Later* list rather t
      han a shape of this rung's. The sphere's tessellation is no longer an open question — it is a claim about the bat
      ch's capacity, settled in xud_shapes.h (see S25's note above).
    - 2026-10-26 note: **a second shape of this item landed as S24** — `xud_draw_cube()` and the `xud_shapes_append_cub
      e()` half it shares with the triangle, in the same `xud_shapes.h/.c` rung; see the *Steps complete (S24 …)* secti
      on at the top of `todo_completed.md`. What the rest of this item owes: the sphere, whose tessellation is a capaci
      ty decision (the shapes rung is PURE and cannot grow the batch, so a sphere's vertex count is a claim about the b
      atch's capacity rather than about the shape), the line/point forms and the wire forms, which need `LINE_LIST`/`POI
      NT_LIST` topologies and so a row of the pipeline list rather than the pipeline's one `TRIANGLE_LIST` row, and `Dra
      wMesh`, which is the mesh / vertex-buffer abstraction in the *Later* list rather than a shape of this rung's.
    - 2026-10-26 note: the frame pair above this item — `xud_begin_drawing()`/`xud_end_drawing()` — landed with S23, so
      the public boundary a shape appends into is no longer missing; see the same section.

## Later

- [ ] Camera + transform math (one perspective 3D camera; the 2D scope was settled out 2026-10-23, see Decided below)
- [ ] Textures + samplers (create, upload via staging copies, mipmaps, sampler variants)
- [ ] Mesh / vertex-buffer abstraction (**one buffer per attribute stream**, SoA, decided 2026-10-23) + handle table
    (index+generation — the public-API currency, **landed 2026-10-24 as S10**, see Completed below)
- [ ] 3D model loading (glTF 2.0 — format decision still open, see Blocked)
- [ ] Text/font rendering
- [ ] Audio wrapper
- [ ] Input wrapper (keyboard/mouse/gamepad over GLFW)
- [ ] Examples set + README
    - 2026-10-31 note: three examples exist today — `hello_context.c`, `hello_triangle.c` and `breakout_3d.c`
      (the first that is a GAME rather than a drawing, and the first that draws more than one shape in a
      frame); the set and the README are still open.

## Decided (2026-10-16)

- Language → **C11**, gcc 16.2.1 primary / clang 22.1.8 second opinion.
- Build → **CMake + ninja**, plus a raw-`gcc` one-liner for the single-TU include path.
- Window/input → **system GLFW 3.5.1** (`pkg-config glfw3`), which in 3.5 gives us `glfwCreateWindowSurface`,
    `glfwGetPhysicalDevicePresentationSupport`, `glfwGetRequiredInstanceExtensions`, `glfwInitVulkanLoader`.
    No `glfwDestroyVulkanSurface` — destroy the surface ourselves.
- Vulkan access → **`dlopen` the loader, no `-lvulkan` link dependency** (Volk-style).
- Validation layers → **on in debug**, requested by enumerating advertised layers, never by name.
- Synchronization → **classic** (semaphores + per-image fences + pipeline barriers + subpass deps), not
    `VK_KHR_vulkan_memory_model`.
- Shaders → **offline SPIR-V only** (`glslangValidator` 11:16.4.0 / `glslc` 2026.3, `spirv-val` in CI). No runtime GLSL.
- Core floor → **`VK_API_VERSION_1_0` + `VK_KHR_SWAPCHAIN`**, everything higher gated on recorded caps.
- GPU memory discipline → **allocate once, suballocate always**, staging via transfer ops.

## Decided since then

- 2026-10-26 — **The transparent variant's three open questions → the raylib-shaped answer for all three** (settled today;
  the *Blocked / needs a decision* entries above are closed by this one, and the tie-breaker the user set is the same every
  time: **whichever option feels like using raylib**). The evidence is the reference headers, quoted with line numbers so
  the choice can be re-checked rather than remembered.

  **D1 — the two pipelines are an ARRAY indexed by the variant constant, and ONE layout for both.** `XudPipeline` grows
  `VkPipeline pipeline[XUD_BATCH_VARIANT_COUNT]` + `VkGraphicsPipelineCreateInfo createInfo[2]` + `VkPipelineColorBlendStateCreateInfo
  blending[2]` + `VkPipelineColorBlendAttachmentState blendAttachment[2]`, and keeps the one `VkPipelineLayout`. Why this is
  the raylib shape: raylib's blending is an **enum-indexed list of modes**, not a pair — `BlendMode` (`reference/raylib/raylib.h`
  918–926) is `BLEND_ALPHA = 0` … `BLEND_CUSTOM_SEPARATE`, nine entries, and `rlgl.h` mirrors it as `RL_BLEND_ALPHA = 0` … (497–505)
  with `GlState.currentBlendMode` an **int** (1068) that a `switch` maps to GL state (2127). A named `pipelineOpaque`/`pipelineTransparent`
  pair would be the wrong shape for a library whose public feel is that list: adding `BLEND_ADDITIVE` — which raylib has and we
  will want, `VK_BLEND_FACTOR_ONE`/`VK_BLEND_FACTOR_ONE` + `VK_BLEND_OP_ADD` being the Vulkan form of `glBlendFunc(GL_SRC_ALPHA, GL_ONE)`
  (rlgl.h 2128) — is a new row in an array, not a new member in a struct and a rename of every site. The batch's `variant` field
  (S16's committed row) is already that index, and `XUD_BATCH_VARIANT_OPAQUE`/`TRANSPARENT` are already the named constants, so the
  array is the only spelling that keeps the batch's index and the pipeline's object one number. **One layout, not two**: the two
  variants differ only in `VkPipelineColorBlendAttachmentState`, which is **pipeline** state, not **interface** state — the sets,
  the bindings and the push range are identical, so a second layout would be a second record of the same interface and every
  `vkCmdBindDescriptorSets` would have to know which of the two it names. `blendEnable = VK_FALSE` (opaque, S15's) and
  `blendEnable = VK_TRUE` with `srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA` / `dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA`
  / `VK_BLEND_OP_ADD` (transparent) is the literal Vulkan form of raylib's default `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`
  + `glBlendEquation(GL_FUNC_ADD)` (rlgl.h 2127, `BLEND_ALPHA` being the **default** mode, raylib.h 918).

  **D2 — the sort rule is the CALLER's append order: no library sort, and no new field on the batch's draw row.** This is raylib's
  own semantics read literally: raylib has no transparency sort anywhere. `SetBlendMode(mode)` (raylib.h 1059) / `rlSetBlendMode(mode)`
  (rlgl.h 2118) does two things — `rlDrawRenderBatch(RLGL.currentBatch)` **flushes the batch when the mode changes** (2121), then the
  state is set — and the draws inside a batch are submitted in the order the caller appended them. So the transparent draws are replayed
  in **recorded** order, the caller draws back-to-front, and `XudBatchDraw` stays `{ firstIndex, indexCount, variant, materialIndex }`
  with no sort key added. The three options considered and why two lose: (a) a sort key on the batch's draw row would be a change to a
  committed, tested S16 rung **and** a field nothing in raylib has an equivalent of — raylib never asks a caller for a depth per draw,
  it asks them to draw in order; (c) a centroid computed by reading the arena back would be a host read of bytes the batch wrote for
  the GPU plus a per-draw cost, and would sort against the caller's back-to-front intent rather than with it; (b) is what raylib does,
  needs no new field, and is the option AGENTS.md's blending rule already names ("which is how raylib's painter's-algorithm semantics
  map over"). The batch's flush decision is the mechanism that makes the order observable at all — `xud_batch_should_flush(requestedVariant,
  requestedMaterial, …)` (xud_batch.c 359) closes a run when either changes, which is the Vulkan form of rlgl.h 2121's flush-on-mode-change,
  and it is already in the tree and already tested.

  **D3 — the switch is a blend-mode STATE call, and the material is named by a handle.** Two halves, both raylib-shaped:
  - **The switch**: the batch's `currentVariant` is set by a **state call** rather than by an argument of the append, because raylib's
    shape is state — `SetBlendMode(mode)` (raylib.h 1059) and `EndBlendMode()` (1059, "reset to default: alpha blending") are calls on
    the drawing state, not arguments of `DrawCube`/`DrawMesh`. The library-rung spelling is `xud_batch_set_variant()` beside the batch's
    own `currentVariant`, and the public `xud.h` spelling that will sit above it is `xud_set_blend_mode()` / `xud_end_blend_mode()` — a
    public-API-surface change, so it lands with the shapes step that will actually need it rather than on its own (AGENTS.md's *Ask before*
    rule for the public surface, and the user's go-ahead covers the decision, not the whole shapes subsystem).
  - **The material**: the batch's `materialIndex` becomes a **material-kind slot index** of the handle layer, and the material slot's
    record holds what a draw needs — the variant bit and the set-1 bindings it names. That is the raylib shape: raylib names the material
    **at the draw call** (`DrawMesh(Mesh mesh, Material material, Matrix transform)`, raylib.h 1632; `DrawMeshInstanced` 1633) and the
    caller **holds** it (`SetMaterialTexture(Material *material, int mapType, Texture2D texture)`, 1657), so in our currency the caller
    holds an `XudHandle` of kind `MATERIAL` (the 2026-10-23 decision's value type, index+generation+kind) and the batch's row stores the
    slot **index** the handle carries, not a bare invented counter. The `XUD_BATCH_NO_MATERIAL` sentinel stays the "no material named"
    state, which is what the handle layer's `XUD_HANDLE_KIND_NONE = 0u` never-stored rule already guarantees a live slot index cannot
    collide with. What the record's exact members are stays the handle layer's step (it is a subsystem: `xud_create_material`/`xud_close_material`
    in `xud.h` + the slot array's material kind), and the decision recorded here is the **naming**, which is the part that was open.
  - **The default stays OPAQUE**, and that is a deliberate divergence from raylib's default, recorded rather than left to be rediscovered:
    raylib's default is `BLEND_ALPHA` (raylib.h 918, `= 0` and commented "(default)"), i.e. blending is ON. Ours is blend-OFF, because
    with a depth buffer and no library sort (D2), blending-by-default is wrong for everything that is not explicitly transparent —
    raylib's default is safe only because GL blends into a color buffer that was already cleared to a known background and its draws
    are painter's-ordered by the caller against no depth write at all. `xud_batch_open`/`xud_batch_reset` set `currentVariant = XUD_BATCH_VARIANT_OPAQUE`
    (xud_batch.c 507/856) and that stays.

  **What is NOT decided by this entry, and stays open**: the material slot's exact record members (above), the blend-mode list growing
  past the two variants (D1 makes it a row in an array, but the list's contents are the shapes/text steps' to want), and whether the
  transparent variant turns depth **writes** off — raylib keeps depth writes on under `BLEND_ALPHA` and gets the overlap artifacts that
  go with it, so keeping them on is the raylib-shaped default and turning them off is a later knob, not this decision.

  **What this entry implemented, the same day (S20)**: D1 and D2 are code, not just decisions — `xud_pipeline.h`/`.c` own the
  variant **array** (one layout, per-row blend-attachment table, per-row `createInfo`, close destroying every row) and the frame
  loop binds the row a draw's variant names beside that draw; the GPU test's last frame runs the transparent state end to end. D3's
  public call (`xud_set_blend_mode`/`xud_end_blend_mode`) is still not in `xud.h` — the test writes the batch's `currentVariant`
  field directly, which is the same tamper-and-restore the tree already uses, and the public spelling lands with the shapes step.

- 2026-10-23 — **Data layout → SoA, no ECS at this level** (settled; AGENTS.md's open question on this is closed).
  One stream per vertex attribute, one `VkBuffer` per stream, one `VkVertexInputBindingDescription` each (`stride` = that
  stream's element size, `offset` 0), one `VkVertexInputAttributeDescription` per attribute on its own `binding`; today's
  four streams `pos`/`uv`/`color`/`normal` = 36 B/vertex **total** split across four buffers instead of interleaved into one.
  **Header facts** grepped today in `/usr/include/vulkan/vulkan_core.h` (1.4.363, no driver call): `VkVertexInputAttributeDescription`
  4267 = `{ location, binding, format, offset }` and `VkVertexInputBindingDescription` 4274 = `{ binding, stride, inputRate }`
  are **not stamped structs** (no `sType`/`pNext` — the physical layer's "stamp the whole array" rule has nothing to stamp here),
  `VkPipelineVertexInputStateCreateInfo` 4377 carries both arrays, `maxVertexInputAttributes` 3614 / `maxVertexInputBindings` 3615
  sit inside the caps' recorded `VkPhysicalDeviceLimits` (3584) so the binding count is caps-gated and never assumed, and one
  `vkCmdBindVertexBuffers` call binds the whole array (`PFN_…` 4655, returns **void** — the zero-state-then-check rule again).
  **Naming trap**: the attribute `format` is a plain `VkFormat` — `VK_VERTEX_FORMAT_*` is **not in these headers** (grep
  `VERTEX_FORMAT` across `/usr/include/vulkan/`: zero hits) and `VK_FORMAT_R8G8A8_UNORM` does not exist (grep `R8G8A8`:
  zero hits), so the 4-byte color
  stream is `VK_FORMAT_R8G8B8A8_UNORM` (byte order R,G,B,A). **Why it fits**: raylib's `Mesh` (`reference/raylib/raylib.h`
  346–372) is already SoA — one array per attribute, `unsigned int *vboId` (371) plural, one VBO per attribute — so the caller-owned
  upload struct the handle-currency decision fixes arrives de-interleaved already: no conversion step, one staging copy per stream.
  **Consequences**: the batch is N stream buffers + one IB (the allocator's suballocation handles the count, no new mechanism), the
  batch's pure-half invariant is "every stream holds the same vertex count" — headless-testable by construction — flush is per stream,
  and the attribute set grows one binding at a time (tangents, bone index/weight pairs, raylib's `texcoords2`) instead of re-laying-out
  a fixed-size vertex. **Probed today** (scratch program linked against `build/libxud.a`, loader → instance → physical, deleted after
  running; same with the validation layer on and off): `maxVertexInputAttributes` 29, `maxVertexInputBindings` 33,
  `maxVertexInputAttributeOffset` 2047, `maxVertexInputBindingStride` 4095, `maxPushConstantsSize` 256 — **not** the numbers the S5.4
  driver note records (32 / 128, `deviceID` 0x400, name "Intel Iris Graphics … (TigerLake-LP GT2 …)") while `vendorID` 0x8086 and
  `apiVersion` 4211042 do match; today's device says `deviceID` 39497 / "Intel(R) Iris(R) Xe Graphics (TGL GT2)", so the driver moved
  since 2026-10-18 and limits must be read off the caps, never off that note. Headroom here: 33 bindings against today's 4.
  **No ECS at this level**: CPU-side records stay one plain struct per object kind inside its slot — the per-kind slot arrays the handle
  decision fixes are the only arrays the library owns and are what an iteration-friendly pass walks; an archetype/system registry in C11
  would be a subsystem (sparse sets, storage migration, system registration) the raylib-shaped public API cannot express and no test here
  would exercise. **Nothing in the tree yet**: no stream buffer, no batch, no `VkPipelineVertexInputStateCreateInfo` — the batch/shapes
  steps write against this decision.

- 2026-10-23 — **Public API handle carries a kind tag** (settled the same day, amends the handle entry below).
  Layout: `typedef struct XudHandle { uint32_t index; uint32_t generation; uint32_t kind; } XudHandle;` — 12 bytes, no padding,
  `static_assert(sizeof(XudHandle) == 12u, "…")` (message-carrying form required — clang 22 rejects a message-less `static_assert`
  under `-std=c11 -Wall -Wextra -Wpedantic -Werror`, see AGENTS.md *Verified loader bring-up*). **One handle type for every kind**:
  the tag, not the C type, is what distinguishes kinds, so a cross-kind mix-up stops being a compile-time type mismatch and becomes
  a **caught runtime error** — exact and cheap. Validation at every public entry point, in this order: `index < slots[kind].count`
  → `generation` equality → `kind` equality; any failure answers `XUD_INVALID_ARG` (the existing `XudResult` code for a bad argument,
  `xud.h` 83–91). The tag is **not** redundant with the array identity — it is what makes the check exact, since a texture handle's
  `(index, generation)` pair could otherwise match a live model slot by coincidence. `XUD_HANDLE_KIND_NONE = 0u` is never stored in
  a live slot, so a **zero-initialised** handle (what a caller holds when it ignores the create call's `XudResult`) is rejected by the
  tag compare before any array lookup happens. Kind values: `XUD_HANDLE_KIND_TEXTURE`/`FONT`/`MESH`/`MATERIAL`/`MODEL` (+ `AUDIOSTREAM`
  when audio lands, extended whenever a new caller-addressable kind lands); library-owned objects callers never name (batch VB/IB,
  per-frame ring, swapchain images, pipelines, descriptor sets) stay tag-less. Constant naming follows the layer convention already in
  the tree (`XUD_DEVICE_QUEUE_MAX_COUNT 2u` at `xud_device.h` 92, `XUD_DEVICE_MAX_DEVICE_EXTENSIONS 8u` at 110, both guarded by
  message-carrying `static_assert`s at 128/131). **Not in the tree yet** (until S10): no `XudHandle`, no `XUD_HANDLE_KIND_*`, no slot
  array exists — that is the `Next` "Handle layer" item above. **In the tree since 2026-10-24**: all of it — see the
  *Steps complete (S10 …)* section at the top of `todo_completed.md` and AGENTS.md's *Verified handle layer facts (2026-10-24)*.
- 2026-10-23 — **Public API currency → index+generation slot handles, not raylib-style pointers**.
  **What raylib does** (re-read today in `reference/raylib/raylib.h`, 6.2): its public types are value structs that embed
  driver names and own sub-objects through pointer arrays — `Texture` (273–279) is `{ unsigned int id; int width, height,
  mipmaps, format; }` with an **OpenGL** texture name, `Texture2D` is only an alias of it (282); `Mesh` (ends 372) carries the
  CPU arrays (`float *vertices/texcoords/texcoords2/normals/tangents`, `unsigned char *colors`, `unsigned short *indices`) plus
  `unsigned int vaoId` and `unsigned int *vboId`; `Model` (ends 433) owns `Mesh *meshes`, `Material *materials`, `int *meshMaterial`,
  a `ModelSkeleton`, `Transform *` poses, `Matrix *boneMatrices`; `Font` (ends 324) owns a `Texture2D texture` **by value** plus
  `Rectangle *recs` + `GlyphInfo *glyphs`; `Material` (ends 392) owns `Shader shader` by value + `MaterialMap *maps`;
  `Shader` (ends 378) is `{ unsigned int id; int *locs; }`; `LoadMaterials` (1653) returns a raw `Material *` array. Ownership/lifetime
  is invisible to the caller and the GL id fields have **no Vulkan counterpart** — a fixed-function-era artifact the "no OpenGL,
  no state machine" rule refuses.
  **Decision**: the public API hands out the house-style opaque handle — `{ uint32_t index; uint32_t generation; }` validated
  against a **per-kind slot array** (textures, fonts, meshes, materials, models, + audio streams when audio lands), each record
  holding the Vulkan handles and the caps-gated variant info. Load-bearing consequences: creation functions fill an out-param
  handle and return `XudResult`, nothing returns a library-owned pointer; teardown is `xud_close_*`/`xudUnload*(handle)` →
  `XudResult` (no `Detach*`-returns-pointer form); handles are value types — copyable, storable, comparable field-wise
  (`a.index == b.index && a.generation == b.generation`), and one kept across a close is **rejected**, not dangling; generation
  bumps on slot reuse (the 2³²-reuse wrap would alias — documented, not ignored); and raylib's habit of hanging **CPU-side**
  source data off the public type does not survive — CPU-side upload data becomes a separate caller-owned input struct
  (`xud_upload_mesh(...)` takes it), the handle addresses **only** the GPU-side object, and read-back goes through accessors.
  **Terminology trap**: "Vulkan-style handle" is **not** Vulkan's own handle — `VK_DEFINE_HANDLE(object)` in these headers
  is `typedef struct object##_T* object;` (vulkan_core.h 26), an opaque pointer, `VK_USE_64_BIT_PTR_DEFINES` defaults to 1 there
  (29–33) and `xud_loader.c:38` asserts `== 1`, so `VkImage`/`VkDevice`/`VkBuffer` are 64-bit pointers that stay **inside** the
  slot records (the existing rule:
  callers never hold a `VkDevice`, `VkSwapchainKHR`, or raw `VkBuffer`). **Not in the tree yet** (until S10): `xud.h` exposes only
  `XudContext**` + `XudResult` today, no handle type and no slot array exists — that is the `Next` item above, and the open
  sub-question (kind tag in the handle vs naming discipline) was settled the same day — see the entry above. **In the tree
  since 2026-10-24**: `xud.h` declares `XudHandle` + the `XUD_HANDLE_KIND_*` values, and `src/xud/xud_handle.h` owns the
  per-kind slot arrays — see the *Steps complete (S10 …)* section at the top of `todo_completed.md`.
- 2026-10-23 — **GLFW backend → Wayland** and **vendored material → committed with its license files**.
  **Wayland**: `GLFW_BUILD_WAYLAND` (glfw/CMakeLists.txt 30, `cmake_dependent_option` ON for `UNIX;NOT APPLE`; the removed
  `GLFW_USE_WAYLAND` switch is a hard error on a stale cache at line 24) → `-D_GLFW_WAYLAND` (glfw/src/CMakeLists.txt 54–55),
  `wayland-scanner` required (`find_program` + `FATAL_ERROR`, 75–77) and the **9** protocol XMLs GLFW ships in `deps/wayland/`
  generated at 100–107, plus `pkg_check_modules(Wayland REQUIRED wayland-client>=0.2.7 wayland-cursor>=0.2.7 wayland-egl>=0.2.7
  xkbcommon>=0.5.0)` (155–160). **All satisfied on this machine** (probed today): `pkg-config --exists` yes for all four,
  `wayland-scanner` at `/usr/bin` (`local/wayland 1.26.0-1`), libs `libwayland-client.so.0.26.0` / `libwayland-cursor.so.0` /
  `libwayland-egl.so.1` / `libxkbcommon.so.0.13.2`; `glfw/src/wl_platform.h` includes `<wayland-client-core.h>` +
  `<xkbcommon/xkbcommon.h>` + `<xkbcommon/xkbcommon-compose.h>` (27–29). `local/libdecor 0.2.5-1` is present but 3.5.1's CMake
  does not use it. `GLFW_BUILD_X11` (29) stays OFF — `x11_*.c`/`xkb_unicode.c` excluded, no X11 path of our own.
  **Commit policy**: `src/external/` is tracked **with** its license files (Volk MIT notice at the end of `volk.h`, GLFW
  zlib/libpng in `glfw/LICENSE.md`) and `src/external/PROVENANCE.md` was added recording versions/licenses/how the copies were
  made (the upstream commit/tag was **not** recorded at copy time — what is recorded is what the files declare: `VOLK_HEADER_VERSION
  366`, `GLFW_VERSION_MAJOR/MINOR/REVISION` 3/5/1). Opposite policy from `reference/raylib/` (never committed, never edited).
  Consequence: ~2.7 MB of upstream code in the repo, and a version bump is a re-vendor commit, never an in-place edit.
- 2026-10-23 — **Loader + window backend → vendored Volk and vendored GLFW** (both already sitting in `src/external/`,
  raylib's own name for vendored deps). **Volk**: `volk.h` + `volk.c`, zeux/volk by Arseny Kapoulkine, MIT (notice at the
  end of `volk.h`), `VOLK_HEADER_VERSION 366`; it declares **842** `extern PFN_vkXxx vkXxx;` entries (counted here), so
  under `VK_NO_PROTOTYPES` every entry point is a **data pointer** filled by `volkLoadInstance`/`volkLoadDevice` — the
  "no Vulkan symbols linked" rule becomes a link-time fact instead of a convention. Bring-up surface: `volkInitialize` /
  `volkInitializeCustom` / `volkFinalize` / `volkGetInstanceVersion` / `volkLoadInstance(Only)` / `volkLoadDevice` /
  `volkGetLoadedInstance` / `volkGetLoadedDevice` / `volkLoadInstanceTable` / `volkLoadDeviceTable`. Its POSIX bring-up is
  `dlopen("libvulkan.so.1", RTLD_NOW|RTLD_LOCAL)` (volk.c 114, fallback `libvulkan.so` 116) — same loader as our own probe,
  `RTLD_LOCAL` not `RTLD_GLOBAL`. Include-order trap (verified): volk.h errors `To use volk, you need to define VK_NO_PROTOTYPES
  before including vulkan.h` when `VULKAN_H_` is already in (volk.h 17–18) and defines the macro itself when it is not
  (25–26), so volk.h comes **before** `<vulkan/vulkan.h>` in every TU. Probed here: `#include "volk.h"` + a `main` compiles
  clean with gcc **and** clang under `-std=c11 -Wall -Wextra -Wpedantic -Werror` even though Volk is 366 and this machine's
  Vulkan-Headers are 363 (`vk_platform.h`, which volk.h includes by default, is present in Arch's headers). Volk here is
  loader-only — no `vk_mem_alloc` reference anywhere in it, so the "our own block allocator" decision still stands.
  **GLFW**: the full upstream 3.5.1 tree at `src/external/glfw` (86 files, 2.2 MB — `include/GLFW/glfw3.h` + `glfw3native.h`,
  35 `.c` files under `src/`, `CMakeLists.txt`, `LICENSE.md`, `CONTRIBUTORS.md`, `deps/wayland/*.xml`), license = the 3-point
  "provided as-is" notice (Copyright © 2002-2006 Marcus Geelnard, 2006-2019 Camilla Löwy). Same Vulkan surface as the system
  copy, re-probed in the vendored tree: `glfw/src/vulkan.c` 264 `glfwGetRequiredInstanceExtensions`, 307
  `glfwGetPhysicalDevicePresentationSupport`, 331 `glfwCreateWindowSurface`; `glfwInitVulkanLoader` at `glfw/src/init.c` 484
  (declared `glfw3.h` 2368); `GLFW_VERSION_MAJOR/MINOR/REVISION` 3/5/1; still no `glfwDestroyVulkanSurface`. The system
  `libglfw.so.3.5` links only `libc`/`libm` (`ldd`) and names `libEGL.so.1`/`libGL.so.1`/`libGLX.so.0`/`libdecor-0.so.0`
  as strings — it `dlopen`s its backend at runtime, so a vendored build must pick Wayland or X11 explicitly.
  **Not done**: `CMakeLists.txt` references neither `src/external/volk.*` nor `src/external/glfw`, nothing in `src/xud/`
  includes `volk.h`, and the landed `xud_loader.c` table is still what runs — the rework is the two `Now` items above.
- 2026-10-23 — **GPU allocator → our own block allocator, no VMA** (not vendored). One allocator owns all buffers/images,
  suballocates over the memory rows the physical layer already recorded, the per-frame ring is one of its regions.
  Header facts grepped in `vulkan_core.h` (1.4.363), no driver call: `VkMemoryAllocateInfo` 3783 is
  `{ sType, pNext, allocationSize, memoryTypeIndex }` — one `VkDeviceMemory` per allocation, the type an **index into
  the caps' `memoryTypes[]`**, stamp `VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO = 5` (212); `VkMemoryRequirements` 3790 is
  `{ size, alignment, memoryTypeBits }`; `PFN_vkAllocateMemory` 4551 returns `VkResult`, `PFN_vkFreeMemory` 4552 returns
  **`void`** (zero-state-before / check-after, like `vkDestroyDevice`). Naming trap: `VK_MEMORY_TYPE` greps to **zero
  hits** in `/usr/include/vulkan/` — a memory type is `VkMemoryType { propertyFlags; heapIndex }` (3521), a pair keyed
  by index, so the caps row the S5.4 note calls "UNIFORM_BUFFER" is a label, not a constant. Why ours: VMA is not
  packaged here, so "use VMA" means vendoring ~200 KB of third-party header, and it hides the create-info assembly the
  headless-test rule leans on behind an interface `tests/` cannot exercise with no device present; the free-list over
  `(memoryTypeIndex, size, alignment)` is exactly that pure half. VMA's headline upgrades are unasked-for and each needs
  its own gate: dedicated allocation is a `pNext` chain entry on `VkMemoryAllocateInfo` (`VkMemoryDedicatedAllocateInfo`
  5704) — it would make the allocator's create-info chain non-empty, the same `pNext` discipline the caps' write-back
  check depends on — and its extension-name constant is **not in these headers** (grep `DEDICATED_MEMORY_EXTENSION_NAME`:
  zero hits), so it could not be requested by name without hardcoding a string. Slot cost: the allocator step adds 2
  device-level slots (`vkAllocateMemory`, `vkFreeMemory`); `XUD_LOADER_SLOT_COUNT` stays 20 today, neither added yet.
- 2026-10-23 — **Scope → 3D only** (no ortho pass, no sprite batch). One perspective camera, one depth buffer, one
  render pass, one batch. The public API carries raylib's 3D half of the shape surface (`DrawCube`, `DrawSphere`,
  `DrawMesh`, `DrawTriangle3D`, `DrawLine3D`, `DrawPoint3D`) and not its 2D half: the screen-space `DrawRectangle`/
  `DrawCircle` family and the `BeginMode2D`/`EndMode2D` pair are out of `xud.h`, not stubbed. Rationale: the opaque/
  transparent pipeline split, the depth-write vs depth-test pair and the sorted-transparent batch rule only mean
  anything with a depth buffer in play, so a 2D pass would be a second pass + a second pipeline set + a second batch —
  a subsystem, not a flag; and raylib's 2D primitives are fixed-function screen quads with no Vulkan shape here. Text
  stays textured quads inside the one 3D pass (billboard or screen-aligned), so the point/glyph shader variant survives
  the decision; the text/font atlas itself is still open below. Recorded in `AGENTS.md` *Settled decisions*.
- 2026-10-22 — **Render pass → explicit `VkRenderPass` + `VkFramebuffer`** (not `VK_KHR_dynamic_rendering`, not both
  gated on caps). Probed today before deciding (scratch probes against `build/libxud.a`, deleted after, no library file
  touched): the driver answers **both** paths — `apiVersion 4211042` (core 1.4), `VK_KHR_dynamic_rendering` and
  `VK_KHR_create_renderpass2` are among the 196 advertised device extensions, and the core
  `VkPhysicalDeviceDynamicRenderingFeatures` answers `dynamicRendering = VK_TRUE` when chained into a fresh
  `vkGetPhysicalDeviceFeatures2`. So a caps gate between the two would leave one half of the renderer never having met
  a driver on this machine, which is against the testing rules — pick one path, write the other down as a gate.
  Explicit wins because: it is the only path that runs at the declared 1.0 + swapchain floor **without touching two
  landed contracts** (the dynamic feature is a *chained* struct, so it would grow `XudDeviceCaps` by a 4th chained
  struct, restate `xud_physical.c`'s "the driver chained something we did not ask for" `pNext` check — probed: `pNext`
  is NULL after the three unchained `*2` getters, non-NULL once we chain — and grow `VkDeviceCreateInfo.pNext`, which
  is NULL today); it matches the design sketch ("begin render pass → clear color+depth") and the subpass-discipline
  rule; it keeps the pure create-info assembly the headless tests lean on; and it is the only path with a **resolve
  attachment**, which is real here — `limits.framebufferColor/Depth/NoAttachmentsSampleCounts` all answer 0x1f (up to
  16 samples). Gate MSAA on those names, **never** on the queue family's `maxImageSampleCount` (1 for the chosen
  family, and that number comes from `VkPhysicalDeviceQueueFamilyProperties2`, a `pNext` chain entry the physical
  layer does not record today — a different limit). Loader slots: the explicit path needs 6 new device-level slots,
  the dynamic path 2; `XUD_LOADER_SLOT_COUNT` stays 20 until the renderer step lands. `vkGetDeviceProcAddr` (PFN
  4540) is the first slot that step must add. Naming trap found while deciding: the recording pair is
  `vkBeginCommandBuffer`/`vkEndCommandBuffer` — **no `Cmd` prefix**, `vkCmdBeginCommandBuffer` does not exist.
  Future gates, not implemented: `VK_KHR_dynamic_rendering` (needs the caps/device `pNext` plumbing above) and
  `VK_KHR_create_renderpass2` as a strict upgrade of this path (+3 command-buffer slots, extension-gated, 1.0 form as
  fallback).

## Blocked / needs a decision

- ~~**The transparent variant's two sub-questions**~~ — **decided 2026-10-26**, see the *Decided since then* entry
  "The transparent variant's three open questions → the raylib-shaped answer for all three": the pipelines are an **array
  indexed by the variant** with **one layout** for both (D1), and the sort rule is the **caller's append order**, no
  library sort and no new field on the batch's draw row (D2).
- Model format: vendor cgltf (glTF 2.0, what raylib does) vs minimal `.glb` reader (ask)
- ~~Handle layer's `record`: … whether the batch's `materialIndex` becomes a handle-layer slot index …~~ — **the naming is
  decided 2026-10-26** (D3 above): the batch's `materialIndex` is a **material-kind slot index**, the caller holds an
  `XudHandle` of kind `MATERIAL` and names it at the draw (raylib's `DrawMesh(mesh, material, transform)`, raylib.h 1632,
  `SetMaterialTexture(Material *material, …)`, 1657), and the slot's record holds the variant bit plus the set-1 bindings.
  What is still open is the record's **exact members** — the subsystem (`xud_create_material`/`xud_close_material` in `xud.h`
  plus the material kind's slot array) is the handle layer's step, and a public create/close pair is a public-API-surface
  change AGENTS.md's *Ask before* rule puts the ask on.
- Audio: vendor miniaudio vs no audio (ask)
- Text/font: own bitmap atlas vs vendored `stb_truetype`/fontstash (ask)
