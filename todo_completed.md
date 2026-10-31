# xudlibc — completed work

Finished-step log, split out of `todo.md` on 2026-10-18 so the active tracker stays short. Newest first. Every entry here is a step that met the commit bar in `AGENTS.md` when it was committed; the notes are kept verbatim, including the mid-step ones — a few of them point at sections (e.g. *Uncommitted state*) that existed only in `todo.md` while that step was in flight and were gone when the step committed. The newest step today is **S24, the box** — the second shape of the shapes
rung, its six faces as data rather than as twelve triangles of code and its geometry checked against the arena rather
than remembered (2026-10-27). The step before it is **S23, the shapes rung and the public drawing API above it** — one
triangle appended atomically into the batch's four streams and its index buffer, the two value types a caller passes do
wn, the public frame pair and the example that draws it (2026-10-26). The step before it is **S22, the resize / recrea
te step** — the reader of the recreate ask S21 left behind, the window's **framebuffer** extent as the answer to that as
k, and the rebuild that follows (2026-10-26). The step before it is **S21, the swapchain's two out-of-band answer codes**
— the second `VkResult` funnel for the acquire/present boundary and the `XudFrame.recreateWanted` flag it leaves behin
d (2026-10-26).

## Steps complete (S24 The box — the second shape of the shapes rung, one step, landed as one commit, 2026-10-27)

- [x] ~~**The box**: `xud_draw_cube()` not in the tree, no shape above the triangle, and the one shape a caller could
    draw was a triangle, so the first thing a frame needed that is not a constant attribute row was still missing~~ —
    **done 2026-10-27** for the box. What is still open of the shapes item is the sphere (a tessellation/capacity deci
    sion), the line/point and wire forms (a topology the pipeline rung does not have) and `DrawMesh` (the mesh rung),
    and the camera + transform math that would make a shape's coordinates world-space rather than clip-space.

  **What landed, rung by rung:**

  - **The six faces as DATA, not as twelve triangles of code** — `xud_shapes.c`'s two tables: the six outward normals
    and the 6 rows of 4 corner SIGNS. A box's corner is at +/-half along ALL THREE axes, so a face's row carries that
    face's own normal sign on the normal's axis rather than a zero there — writing the zero instead is what makes the
    24 corners collapse to the 12 points inside a box rather than its 8 corners, and the tests area's plane check is
    what caught that on the first pass. The corners are signs rather than coordinates because nothing about the caller's
    centre or extents is in the table, and the only axis-aligned box shapes there are are the 8 sign triples.
  - **One winding rule for all six faces, and the rule is a fact about the tangent axes rather than a per-face excepti
    on**: for a face whose tangent axes are u and v the corners are -u-v, +u-v, +u+v, -u+v and u x v IS the outward
    normal exactly, not merely parallel to it. The u and v are chosen SIGNED per face for that reason — which is what
    lets the same four-corner order cover all six faces instead of a per-face flip, and what makes the tests area's
    cross-product check a check of the table rather than a check of the code that reads it.
  - **24 vertices, not 8, and that is the shape's own claim**: a box's corner belongs to three faces and wants three
    different normals, so the corner is appended three times. The batch has no per-face attribute and no index into a
    normal table, so the vertex count is the shape's, not the topology's — and the header's own doc says four per face
    so a face's normal belongs to that face alone.
  - **The append is atomic in the same shape as the triangle's, and it checks BOTH capacities** — a box costs 24 ve
    rtices and 36 indices, and 36 is exactly the 3n/2 `xud_batch_layout()` sizes the index headroom at for 24 vertice
    s, so a box is the closed-mesh case of that bound rather than a shape that needs headroom beyond it. Both bounds
    are checked rather than only the vertex one because **an index names a live vertex rather than a new one**: a ba
    tch whose existing content reuses vertices spends more than that 3n/2 and can be full on its indices while it st
    ill has twenty-four vertices' worth of room, and a refusal that checked only the vertices would write a box's 24
    vertices into a batch that cannot name them. The header's static_asserts pin 36 == 3n/2 and one box inside the i
    nitial capacity, so drawing one box asks nothing of the grow path.
  - **The one context guard the public draw calls share is one function, not one block per call** — `xud_shapes_conte
    xt_batch()` is file-local because it is not the layer's contract: the contract is the shapes and the public calls,
    and this is the half every public call above this rung needs, reaching the same two fields of the same owner and an
    swering the same two codes.
  - **The tests area checks GEOMETRY, not a remembered table** — the positions are read off the arena and checked to
    sit half a box out along the axis their own normal points down and inside the other two extents; the normals are
    checked to be unit axis vectors; both triangles of all six faces are checked to have cross products along that face
    's OUTWARD normal; and every face's six indices are checked to name its own four corners with all four named. The
    extents the test uses are dyadic (1, 0.5, 2) so every comparison is exact rather than a tolerance standing in for
    a fact, and the test's shape is a BOX (three different extents) rather than a cube, so a test that assumed eight
    symmetric corners would be a test of the wrong shape.
  - **The index claim is "all four named", not "each named twice"** — the two triangles of a quad share the 0-to-2 dia
    gonal, so the two corners at its ends are named twice and the other two once each. Saying "each named twice" is wha
    t the first pass asserted and what the arena answered was false; the face is closed either way and the check that
    holds is the one the geometry says.
  - **The GPU half draws the box in the same frame as the two triangles** — 30 vertices and 42 indices in one open bat
    ch, replayed through the same one triangle-list pipeline, which is the reason a box is a shape this rung can draw
    today and a line is not. The signal is still the counters rather than a screenshot: the frame's one pipeline is th
    e unlit solid one and a box's face normal is read by no shader yet, so a box and a triangle are the same pixels to
    a screenshot and the counters are what say the 24 vertices and 36 indices are in the batch the frame drew.

  **Checked before commit:** `gcc -fsyntax-only -std=c11 -Wall -Wextra -Wpedantic -Werror` clean over `src/xud/*.c`;
  `./scripts/format.sh --check` clean; the headless run 2433 checks 0 failures; `XUD_GPU_TESTS=1` 3132 checks 0 failu
    res and validation-clean (no VUID line, the diagnostics area's own synthetic probe lines aside); the `-fsanitizer`
    build green with `ASAN_OPTIONS=detect_leaks=0` and its 72-byte driver leak unchanged from the tree as it stood bef
    ore this step (probed with `git stash`, and identical with `VK_INSTANCE_LAYERS=` set empty, so it is the dlopen'd
    driver's and not the library's); the clang 22 build warning-free and green headless and GPU.

## Steps complete (S23 The shapes rung and the public drawing API above it — one shape, the frame pair, and the example that draws it, one step, landed as one commit, 2026-10-26)

- [x] ~~**The shapes rung and everything above it**: `xud.h` declaring **no draw call at all**, no value type a caller
    passes down and no frame boundary, so nothing of a caller's concern could be drawn and `examples/` had one program,
    `hello_context.c`, which draws nothing~~ — **done 2026-10-26** for one shape and the frame pair. The rest of raylib's
    3D shape family (`DrawCube`, `DrawSphere`, `DrawLine3D`, `DrawPoint3D`, `DrawMesh`) stays open in `todo.md`'s *Next*,
    and so does the camera + transform math that would make a shape's coordinates world-space rather than clip-space.

  **What landed, rung by rung:**

  - **A new rung, `src/xud/xud_shapes.h/.c`**, between the batch and the public API — the same split raylib makes (rayli
    b's `DrawTriangle3D` lives in `rshapes.c`, above `rlgl`'s batch), and the reason the file holds **two** functions ra
    -ther than one: `xud_shapes_append_triangle()` is the half a test can run against a batch it built itself, with no co
    ntext and no driver in sight, and `xud_draw_triangle_3d()` is the public call whose whole job is to reach the contex
    t's renderer's batch and hand that half the shape. The public call sits in this file rather than in `xud_core.c` for
    the same reason raylib's sits in `rshapes.c` — the shape IS the public call's job, and `xud.h` is the declaration of
    it rather than its implementation.
  - **The append is atomic, and that is the whole reason the counters are read before anything is written.** A triangle
    costs three vertices and three indices, so the fit check is against **both** of the batch's capacities, and a shape
    that does not fit is refused before a single byte of it is written — `XUD_OUT_OF_MEMORY`, the batch's own code, read
    through `xud_batch_stats()` rather than off the fields. A shape that wrote part of itself before refusing would be a
    batch whose live count says six and whose arena holds two and a half triangles, which is a state a frame loop would
    then draw half a triangle from. The GPU half and the pure half both check the counters **after** the refusal, not on
    ly the code.
  - **The two constant attribute rows are named constants in the header, not literals in the source** — the flat +Z nor
    -mal and the zero uv. The batch's appends refuse a NULL array rather than zero-filling one, so a stream this shape h
    as nothing to say about still has to be handed something, and the reason it is what it is is stated beside each cons
    -ant. The colour is **not** a constant: it is the caller's, and it is the batch's COLOR stream's four bytes in the ca
    ller's R,G,B,A order, so the public `XudColor` and the stream's element are one number rather than a conversion — ch
    ecked against `XUD_BATCH_STREAM_ELEMENT(XUD_BATCH_STREAM_COLOR)` rather than asserted in prose.
  - **The two value types a caller passes down are raylib's own two**, `XudVector3` (raylib.h 222) and `XudColor` (253),
    and they are in the opposite bucket from the handle `xud.h` already declares: a handle ADDRESSES an object the libra
    ry owns and a caller never reaches into, and these two carry the bytes a caller hands DOWN for the library to write
    into the batch's streams. Neither has a lifetime, a close or an error code of its own.
  - **The public frame pair is the raylib-shaped boundary above the context** — `xud_begin_drawing()`/`xud_end_drawing()`
    over raylib's `BeginDrawing`/`EndDrawing` (raylib.h 1048/1049), and the order the pair states is the order raylib's
    own comment states for that pair (raylib.h 1100: "By default EndDrawing() does this job: draws everything + SwapScre
    enBuffer() + …") — the draws append and the end of the frame is what submits and presents them. Nothing here is a st
    ate machine and nothing here is a GL call: the pair maps onto the renderer rung's one frame call and the batch rung'
    s one reset, and **neither half of the pair calls a driver function at all**.
  - **`xud_end_drawing()` is the one place above the frame loop a public caller can act on the recreate ask**, which is
    what closes S21's and S22's open question from the public side. The swapchain is the context's, so a recreate replac
    es every image, every framebuffer row and every fence index the renderer holds, and it has to happen between two dr
    aws rather than inside one — so the closing half of the pair runs the frame and then `xud_renderer_resize()`, which
    is the reader of `XudFrame.recreateWanted` and answers `XUD_ALREADY` when nothing was asked for, which is what makes
    calling it every frame cheap rather than a rebuild every frame. The resize is not this rung's to refuse: a rebuild t
    hat failed is reported.
  - **`hello_triangle.c` is the second example and the first thing that draws anything**, and it includes `xud.h` only —
    which is the point of the public header: user code names a context, a frame boundary and a triangle, and never a VkD
    evice, a swapchain, a pipeline, a batch, a buffer or a fence. It draws three frames, so the loop is visible: the fir
    st frame's batch is empty at open, the frame that completed resets it, and the next `begin_drawing()` opens a bound
    -ary on an empty batch again.
  - **A new test area, `tests/xud_test_shapes.c`**, one TU per the repo's test-split rule, running above the batch area
    and below the core area, which is the layer order: the shape is checked against a batch the area laid out itself (th
    e same host-arena stand-in the batch area uses, since the batch's own mapping is a driver fact), and the public call
    s are checked against a real context inside that area's own GPU half. The pure half checks the **bytes**, not only t
    he counts — the caller's four colour bytes in the caller's order at the COLOR window's offset, the header's flat +Z
    at the NORMAL window's, the header's zero uv at the UV's, the caller's first point as the first row of the POS windo
    w's, and the three indices as 0,1,2 at the index buffer's offset — because a count-only check would pass a shape tha
    t wrote the wrong bytes.

  **Verified on this machine (2026-10-26):** the headless run is **2399 checks / 0 failures** (up from 2366, the 33 new
  ones this area's pure half adds) and the `XUD_GPU_TESTS=1` run is **3086 / 0** (up from 3053), validation-clean throug
  hout — the GPU half draws a real triangle through a real pipeline three frames in a row and the messenger hears nothi
  ng about it, which is the signal the repo's validation rule asks for rather than "it didn't crash". `./build/hello_t
  riangle` answers `XUD_OK` for the query, the bring-up, all three frames' three calls each and the teardown, and exits
  0. The `-DXUD_SANITIZERS=ON` build reports **the same 72 bytes / 3 allocations as before this step** — the 8-byte libd
  ecor/libgdk one GLFW `dlopen`s per window and the validation layer's 32 bytes per `vkCreateDevice`, both outside this
  tree, neither of them new here and no suppression installed to make a run look clean. `clang` 22 builds the two new TU
  s warning-free, and `./scripts/format.sh --check` is clean over `src/xud tests examples` after `./scripts/format.sh`.
  `nm -u` on the new example lists no `vk*`/`glfw*` symbol and `ldd` lists no `libvulkan`/`libglfw` — the two rules hol
  d of the new example, not only of the library.

## Steps complete (S22 The resize / recreate step — the reader of the recreate ask, the framebuffer extent as the answer, and the rebuild that follows, one step, landed as one commit, 2026-10-26)

- [x] ~~**The resize / recreate step**: `XudFrame.recreateWanted` having **no reader** anywhere above the frame loop, so
    the pair `VK_SUBOPTIMAL_KHR` / `VK_ERROR_OUT_OF_DATE_KHR` being a frame that runs and asks with nothing acting on the
    ask~~ — **done 2026-10-26**.

  **What landed, rung by rung, bottom up:**

  - **The window rung gains the two GLFW calls a resize needs**: `xud_window_resize()` over `glfwSetWindowSize` and
    `xud_window_framebuffer_size()` over `glfwGetFramebufferSize`. The resize writes the requested extent into
    `window->width`/`window->height` rather than reading it back, because `glfwSetWindowSize` returns **void** — the
    zero-state-before-the-call rule this tree follows for a void call, applied to a record instead of a handle. The
    bounds are the window layer's own `XUD_WINDOW_MIN_EXTENT`/`XUD_WINDOW_MAX_EXTENT`, and they are checked **before**
    GLFW is handed an `int` that cannot hold the extent: GLFW's own Wayland `resizeWindow` clamps to 1 rather than refu
    sing, so a zero extent that reaches it is a zero-extent swapchain waiting one call away. An extent already in hand
    is `XUD_ALREADY`, the rule `xud_swapchain_recreate()` follows one rung up. `xud_window_framebuffer_size()` refuses
    a non-positive extent rather than writing one, because GLFW's Wayland half zeroes both out-params before it
    dispatches and a 0/0 answer cannot make a valid swapchain.
  - **The framebuffer extent, not the window extent, is the ask's answer** — and that is the whole reason the pair of
    calls exists separately. GLFW's Wayland half scales `wl.width`/`wl.height` into `wl.fbWidth`/`wl.fbHeight` by the
    fractional scale (`(w * scalingNumerator + 60) / 120`) or the buffer scale, so on a scaled session the two pairs
    differ, and a swapchain created at the **requested** pair is a swapchain the compositor calls out of date on every
    acquire, which is a resize loop rather than a resize. The one fact that holds at every scale factor, and the one
    the GPU test pins, is that the framebuffer extent **moves with** the window.
  - **The render-pass rung gains `xud_renderpass_recreate_depth()`**, and the depth attachment is recreated because
    `VUID-VkFramebufferCreateInfo-attachments-02702` makes every framebuffer attachment the framebuffer's own extent:
    a new swapchain extent forces a new depth buffer. The order is create-new-then-retire-old, so a recreate that fails
    leaves the pass exactly as it was rather than as a pass whose description names an image that was never made — the
    new image, its one view and its carve of allocator memory are made first, the old row is retired through
    `xud_renderpass_retire_depth()` after, and a failure after the new row exists restores the old one into the pass. The
    shared teardown is why the teardown split exists: `xud_renderpass_retire_depth()` (view before image before release,
    handles cleared before the destroy calls) is used by the whole-pass teardown and by the recreate's failure paths
    alike, so the order is one owner's rather than restated. The rung calls `xud_device_wait_idle()` before it starts,
    because the frame loop may still have a submission or a presentation reading the very depth buffer being replaced.
  - **The renderer rung gains `xud_renderer_resize()`**, the reader of the flag, and it runs **between draws** rather
    than inside one: the swapchain is the context's, a rung below, and a recreate retires every image, every
    framebuffer row and every fence index this rung holds, so mid-draw is a state that cannot be rebuilt. The order is
    the one that is correct — frame closed, framebuffer array closed, the context's swapchain recreated at the
    framebuffer extent, the pass's depth image recreated at it, rows reopened against the new images and the new depth
    view, frame reopened with one fence per new image — and the batch, the pipeline and the descriptor are **not**
    touched, because none of them names an extent, so a resize that rebuilt them would be more work than the resize
    needs. Two refusals fire before anything is torn down: a count that no longer matches the swapchain in hand is
    `XUD_UNSUPPORTED` (the same mismatch `xud_frame_draw()` reports one rung down, read here before a rebuild would
    make it worse), and a window with no framebuffer extent to give is `XUD_INVALID_ARG` rather than a rebuild at an
    extent of 0. A resize whose framebuffer extent already matches the swapchain clears the flag and answers
    `XUD_ALREADY`, which is what stops one ask becoming an endless recreate loop.
  - **The swapchain rung's recreate owes a guard it did not have**: `vkCreateSwapchainKHR`'s `oldSwapchain` argument
    retires the old handle as part of the creation, and the rule a retired swapchain owes is the one
    `VUID-vkDestroySwapchainKHR-swapchain-00965` states for the explicit destroy — *all uses of the presentable images
    acquired from swapchain must have completed execution*. A frame that was submitted and presented but not yet
    finished is a use of those very images. This is not a hypothesis here: probed on this machine, a resize taken
    after a presented frame answered exactly that, `vkDestroySwapchainKHR(): can't be called on a VkSwapchainKHR that
    is currently in use by a VkQueue`, and the guard added (wait-idle after the surface queries, before the create,
    and the `vkDeviceWaitIdle` row added to the rung's own table check) is what made it clean. A recreate whose guard
    cannot run has torn nothing down.

  **The tests, and what each half proves:**
  - The **window** area gains 9 headless checks for the two calls' guards (null window, null out-params, the extent
    bounds, a window with no GLFW window behind it) and 11 GPU checks that the framebuffer extent is readable, positive
    and **moves with** the window across a real resize, plus that the window is resized back so the rest of the area
    runs at its original extent.
  - The **renderpass** area gains 12 headless checks for `xud_renderpass_recreate_depth()`'s guards — every argument
    refusal, the `XUD_ALREADY` row (the extent ask is already satisfied), the nil-table row (an image this layer cannot
    destroy is an image it must not create, which is why the destroy rows are checked before the create rows at all), a
    zero extent, a handle-less swapchain, a handle-less device (reported `XUD_UNSUPPORTED`, the rule every driver rung
    here follows) and a pass with no depth row (a broken pass, refused rather than half-recreated).
  - A **new area file, `tests/xud_test_renderer.c`**, is the resize's own rung: 13 headless checks that every guard
    fires before a driver call, against a renderer whose five rungs are all present and whose loader rows are all nil,
    which is the whole point of testing this half with no device present. Its GPU half runs the rebuild end to end
    against a completed context: a frame drawn so a presentation exists, the ask set the way the frame area tampers
    its own fields (this machine's compositor answers `VK_SUCCESS` for every acquire, so the funnel never sets the flag
    here — the rebuild is the half that has to work either way), the window resized, the rebuild run, and then the
    facts that say the new set is a set the frame loop can actually run against rather than one that happened to
    survive: the swapchain at the framebuffer extent, the depth image and view and the new range live, the rows and
    the fences and the count the draw hands down all matching the **new** swapchain, a reopened row built at the new
    extent, the reopened rows naming the **new** depth view rather than the retired one, the reopened frame naming the
    new swapchain, the flag cleared, the reopened loop's frame count starting fresh at 0, the untouched rungs still
    live, a frame drawing at the new extent, and a second call answering `XUD_ALREADY`.
  - The area is wired in one place per the testing rules: one line in `tests/xud_test_main.c`, one line in
    `CMakeLists.txt`'s explicit source list (between the frame area and the allocator area, the ladder slot the
    renderer sits in), one entry point declared in `tests/xud_test_harness.h`.

  **The run, measured**: headless 2330 → **2366** checks, GPU 2938 → **3016**, both 0 failures and validation-clean
  (the renderer area's own GPU check asserts the validation layer's error count stays 0 across the rebuild and the
  frame after it, so the old rows dying before the old images were retired by the recreate, and the old depth view
  dying before the old depth image, are a checked fact rather than a claim). The `-DXUD_SANITIZERS=ON` build reports
  72 bytes / 3 allocations headless and 1376 bytes / 70 allocations GPU against the pre-change tree's 1336 / 68 — the
  delta is exactly the two 8-byte libdecor/libgdk allocations GLFW's `glfwInit` leaks per window opened, the class the
  log already documents as living wholly outside this tree, and the new area opens one more window. The clang 22
  second-opinion build is warning-free and answers the same 2366 / 0.

  **AGENTS.md** gains the rule under *Vulkan rules*, beside the swapchain bullet it belongs to: a recreate is a rung
  above the frame loop, asked for by the framebuffer extent rather than the window's, in the one order that is
  correct, and it owes the same wait-idle guard a destroy does.

  **What stays open**: the resize is **rung-level**, not public — `xud.h` still declares no call that names a resize,
  and the public spelling (and the `glfwSetFramebufferSizeCallback` / `glfwSetWindowSizeCallback` pair that would make
  a compositor's resize reach it on its own) is a public-API-surface change that stays an ask. Nothing in `examples/`
  resizes yet. And the flag's producer is still unexercised on this driver: this machine's Wayland session answers
  `VK_SUCCESS` for every acquire, so the two out-of-band codes' GPU path is a table the headless run pins rather than
  a behaviour this run observes.

## Steps complete (S21 The acquire's and the present's positive answer codes — a second VkResult funnel for the swapchain boundary, and the recreate flag it leaves on the frame, one step, landed as one commit, 2026-10-26)

- [x] ~~**The acquire's positive answer codes**: `VK_SUBOPTIMAL_KHR` / `VK_ERROR_OUT_OF_DATE_KHR` being mapped to `XUD_VULKAN_ERROR` by the general funnel, so a frame whose acquire answered "usable image, recreate the swapchain" was **refused** rather than drawn, presented and flagged~~ — **done 2026-10-26**.

  **What the item claimed and what is true today** (corrected in this entry rather than in the item, so the log keeps bot
  h readings): the item as written in `todo.md` said the GPU test's 4th `vkAcquireNextImageKHR` answered `VK_SUBOPTIMAL_KH
  R` here. Re-probed today against the pre-fix tree and again against the fixed one, with a temporary `fprintf` beside th
  e acquire (probe added, run, removed, tree re-checked clean): all four acquires answer **`VK_SUCCESS`** and the image in
  dices are 0,1,2,3, so on this machine's Wayland session the two codes are not what the driver answers for a four-frame
  run and the tree has been green over the loop for a different reason than the item thought. The rule the step installs i
  s therefore a **spec** fact plus a **table** the headless run checks, not a GPU-observed behaviour here — and the GPU-o
  bserved half of the claim may have been a window that had been moved or rescaled between the runs, which is exactly the
  condition the two codes answer. What is NOT in question is that the general funnel's mapping is wrong for these two cal
  ls: it is, and the next step that resizes the window will hit it.

  **What landed**:
  - **A second funnel, not a new row in the first one.** `xud_vulkan.h`/`.c` gain `xud_swapchain_result_from_vk(VkResult,
    uint32_t* outRecreateWanted)`, which maps `VK_SUCCESS` → `XUD_OK`/flag 0, `VK_SUBOPTIMAL_KHR` and `VK_ERROR_OUT_OF_DA
    TE_KHR` → `XUD_OK`/flag 1, and **everything else** → `xud_result_from_vk(vkResult)`/flag 0 — so the two funnels differ
    in exactly two rows and nothing else, including for the two swapchain-adjacent codes that are not of this pair
    (`VK_ERROR_SURFACE_LOST_KHR`, `VK_ERROR_INCOMPATIBLE_DISPLAY_KHR`, which name a lost surface and an unusable display
    mode and stay refusals). The flag is written on **every** path, error paths included, before the switch rather than on
    ly inside the rows, because an out-param undefined on a path is a question a caller cannot ask; a NULL flag is `XUD_IN
    VALID_ARG`, refused before any mapping is made. The general `xud_result_from_vk()` is untouched and still calls both
    codes `XUD_VULKAN_ERROR`, which is the right answer everywhere outside those two calls — one table with exceptions wo
    uld make the exception a caller's judgement rather than a fact of the entry point, which is why there are two.
    Three message-carrying `static_assert`s in `xud_vulkan.h` keep the pair two distinct codes, neither of them `VK_SUCCE
    SS`, or the funnel's `OK` row would be a lie about one of them.
  - **The flag lives on the frame, not on the result.** `XudFrame` gains `uint32_t recreateWanted`, written 0 at `xud_fra
    me_open()`, cleared 0 at `xud_frame_close()`, set 1 by `xud_frame_draw()` when the **acquire** or the **present** ans
    wered the pair. It is a **request, not an action**, and that is the whole point: this rung cannot recreate — the swap
    chain is the context's, a rung below, and recreating it retires every image, every framebuffer row and every fence in
    dex this rung holds — so the rung that observed the answer records it and the resize step above acts on it. Sticky ra
    ther than rewritten per frame, because "a recreate is wanted" stays true until someone makes it: a clean acquire on t
    he next frame does not un-want the recreate the previous one asked for.
  - **Both call sites changed**, and the present's answer changed with the acquire's: `vkQueuePresentKHR` is documented t
    o answer the same two codes, and for it both mean the presentation still happened — returning an error there would be
    a frame that presented and then reported it had not. The bounds check on the acquired index stays where it was, aft
    er the funnel rather than before it, because the out-of-date answer's `pImageIndex` is still written and still names a
    n image this swapchain has.
  - **The tests**: the vocabulary area gains 23 checks that pin the two tables against each other — the pair is `XUD_OK` +
    flag 1 through the swapchain funnel and `XUD_VULKAN_ERROR` through the general one, every other code delegates to th
    e general mapping with the flag written 0 on the error path (checked by pre-setting the flag to 1, so "written" is a
    fact rather than a `calloc` coincidence), and a NULL flag is refused. The GPU area gains 2 checks that the flag is two-
    state rather than a count, and that the loop opens with it clear. Headless went 2307 → 2330 checks, the GPU run 2913
    → 2938, both with 0 failures and validation-clean; the `-DXUD_SANITIZERS=ON` build reports the SAME 72 bytes / 3 all
    ocations headless and 1336 bytes / 68 allocations GPU as the pre-change tree (probed by stashing this step, re-runni
    ng both, then popping), i.e. only the allocations the log already documents as living wholly outside this tree — the
    8-byte libdecor/libgdk one GLFW `dlopen`s and the 32 bytes per `vkCreateDevice` inside the validation layer — and no
    thing this step adds.
  - **AGENTS.md** gains the rule under *Vulkan rules*, beside the swapchain bullet it belongs to: the two codes are a fra
    me, not a refusal, and the two funnels are what keep that a fact of the entry point rather than a caller's judgement.

  **What stays open**: the flag has **no reader yet** — the resize/recreate step is the next item and is what will act o
  n it (recreate the swapchain, then the framebuffer array and the fence array with it, at the next acquire rather than m
  id-frame). Nothing in `src/xud/` above the frame loop reads `recreateWanted` today, and saying so is the point: the st
  ep installed the request half of a decision whose action half is still a step away.

## Steps complete (S20 The transparent variant — a pipeline per row of the batch's variant list under one layout, the frame loop binding the row a draw names, one step, landed as one commit, 2026-10-26)

- [x] ~~**Batch consumers (part 4 of 4)**: the **transparent** pipeline variant + the sorted-draw rule~~ — **done 2026-10-26**.
    What stays open in the item, and stays in `todo.md`: the handle layer's material **record** (the naming is decided, the
    record's members and `xud.h`'s create/close pair are not) and the transient ring's still-caller-less pair.

  **The two questions, and the answer the same day's *Decided since then* entry records for them** (D1 the pipeline list,
  D2 the caller's append order — the tie-breaker being "whichever option feels like using raylib", with the evidence quoted
  there by line number so the choice can be re-checked rather than remembered). This step is the first **code** of those
  decisions:
  - **D1 in code — the pipeline rung owns a LIST, not a pair.** `XudPipeline` grows `VkPipeline pipeline[XUD_BATCH_VARIANT_COUNT]`
    plus the per-row `createInfo`/`blending`/`blendAttachment` arrays, and keeps the **one** `VkPipelineLayout`. `xud_pipeline_fill_state()`
    fills the blend attachment from a **per-row table** — opaque `{ VK_FALSE, ONE, ZERO }`, transparent `{ VK_TRUE, SRC_ALPHA,
    ONE_MINUS_SRC_ALPHA }`, both `XUD_PIPELINE_BLEND_OP` = `VK_BLEND_OP_ADD` — the literal Vulkan form of raylib's default
    `BLEND_ALPHA` (`glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)` + `GL_FUNC_ADD`). `xud_pipeline_fill_create_info()`
    takes the variant and refuses one the list has no row for; `xud_pipeline_open()` creates one pipeline per row and records
    them; `xud_pipeline_close()` destroys **every** row through a file-local `xud_pipeline_destroy_rows()`, so a partial
    creation leaks nothing. The static asserts moved to the array's own size (`sizeof(((XudPipeline*)0)->pipeline) /
    sizeof(VkPipeline) == XUD_BATCH_VARIANT_COUNT`) plus `XUD_BATCH_VARIANT_COUNT >= 2u`, which is what keeps the array's
    length and the batch's variant count one number rather than two that happen to agree.
  - **The frame loop binds per DRAW ROW, not per frame.** The once-per-frame `vkCmdBindPipeline` is gone; the loop bounds
    `pipeline->pipeline[drawRow.variant]` beside each row it replays, and refuses a row whose variant is off the end of the
    list (`drawRow.variant >= XUD_BATCH_VARIANT_COUNT` breaks the replay, the same shape as the `firstIndex + indexCount >
    vertexCount` rule beside it). A bind is command-buffer state, so the last bind before a draw is the one that draw uses —
    the Vulkan form of rlgl re-selecting state per batch, and the reason the per-frame bind was wrong rather than merely
    coarse: with one bind the variant the row names would have been decoration on a row that always drew through the opaque
    pipeline.
  - **D2 needed no code**, and saying why is the point: the sorted-draw rule is the caller's append order, the library never
    sorts, and no sort key is added to `XudBatchDraw` — the row array **already is** the append order, and `xud_batch_should_flush()`
    (xud_batch.c 359, committed and tested at S16) is the mechanism that makes a variant change close a run at all, which
    is the Vulkan form of rlgl's flush-on-mode-change. A sort would have been a change to a committed, tested rung and a
    field nothing in raylib has an equivalent of.
  - **The GPU test runs the transparent row end to end**: the last frame of the loop sets the batch's `currentVariant` to
    `XUD_BATCH_VARIANT_TRANSPARENT` — the field, because the public spelling is `xud_set_blend_mode`/`xud_end_blend_mode` in
    `xud.h` (D3, and a public-API-surface change that lands with the shapes step that will want it), so writing the field is
    the same tamper-and-restore the `framebufferCount` check above already does — and the checks say the frame that names the
    transparent variant **drew**, the transparent row of the list is a live handle, the two rows are **two** handles (so the
    variant is a pipeline choice, not a flag on one pipeline), and the frame's batch reset moved the variant back to the
    opaque default, which is what the batch's own reset rule states and this frame is the only thing that ran it. The frame
    count stays `XUD_TEST_GPU_DRAW_FRAMES`, which is what keeps the acquire count the run has always had — the 4th acquire on
    this machine answers `VK_SUBOPTIMAL_KHR`, and that is written up as a *Next* item in `todo.md` rather than absorbed here.
  - **The headless half** asserts the two rows differ in exactly the variant field: opaque `blendEnable` differs from transparent
    `blendEnable`, the transparent factors are `SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA` and the opaque ones stay `ONE`/`ZERO` under
    both rows, the color write mask is full under both, the transparent `pAttachments` points at the transparent row, and
    `fill_create_info` now takes a variant with a refusal for one off the end of the list plus a check that the `pColorBlendState`
    it points at is the variant's own row. `xud_pipeline.h`'s file-top comment states the list and the one-layout rule with its
    reason (the variants differ in **pipeline** state, not **interface** state, so a second layout would be a second record of
    the same interface and every `vkCmdBindDescriptorSets` would have to know which it names), and `xud_batch.h`'s variant
    comment now says BOTH rows exist today rather than "OPAQUE is the only one the pipeline rung builds today" — the stale
    wording this step retires. `AGENTS.md`'s pipeline rule names the two-row list and the per-row bind, and its frame-loop
    order moves `vkCmdBindPipeline` out of the once-per-frame line and beside the push/draw pair.
  - **The bar, met**: `gcc -fsyntax-only -std=c11 -Wall -Wextra -Wpedantic -Werror -I src/xud -I src/external -I build/gen` clean
    (and the same under clang 22), `./scripts/format.sh --check` green, headless `./build/xud_test` **2307 / 0**, GPU run
    **2913 / 0** validation-clean, clang build **2307 / 0** and **2913 / 0**, sanitiser builds green with the known external-only
    leak set, `ar -t build/libxud.a | wc -l` still **40**, `nm -u build/xud_test | grep -E "vk|glfw"` still empty, `ldd` still
    answers only `libwayland-egl.so.1`.

## Steps complete (S19 The push-constant row — `vkCmdPushConstants` beside every recorded draw, the vertex stage placing the vertex by `viewProj * modelMatrix`, one step, landed as one commit, 2026-10-26)

- [x] ~~**Batch consumers (part 3 of 4)**: `vkCmdPushConstants` into the range the descriptor rung already wrote, and the
    shader reading the **push-constant model matrix**~~ — **done 2026-10-26**. What stays open in the item, and stays in
    `todo.md`: the **transparent** pipeline variant + the sorted-batch rule, the handle layer's `XudHandleSlot.record`, and
    the transient ring's still-caller-less pair.

  **The channel, and why it is per draw rather than per frame**: a push-constant range is command-buffer **state that the
  last push before a draw bound**, which is exactly what makes it the per-draw channel and set 0 the per-frame one — the
  same layout, read from the two ends. The descriptor rung had already declared and gated the range (`xud_descriptor_fill_pu
  sh_range()`, stageFlags VERTEX|FRAGMENT, offset 0, size `XUD_DESCRIPTOR_PUSH_SIZE` = 80 = `sizeof(XudPushRow)`, gated on
  `limits->maxPushConstantsSize` against the 128-byte core floor) and nothing consumed it; S18's honest boundary said pushing
  a range a stage does not read changes nothing about what is drawn, so the call and the read land together rather than one
  before the other.

  **The push names the layout's OWN recorded range, not constants of the loop's**: `descriptor->pushRange.stageFlags/offset/size`
  are the arguments, beside `pipeline->layout` — the same discipline the set-0 bind one line above follows with the same
  field (`pipeline->layout` rather than a layout of the loop's), and the reason is one sentence: the layout the pipeline was
  created with is the layout the set and the range were allocated against, so naming the recorded row is what makes the two
  agree rather than two records that happen to match. The validation layer checks that agreement (`VUID-vkCmdPushConstants-
  stageFlags-06674`/`offset-06675`/`size-06676`: the call's stage flags must be a **subset** of the layout's for a range, and
  offset+size must lie inside it), so a loop that invented its own constants would be a run that only looked right until the
  layer moved. The new refusal beside the four descriptor rows above is `descriptor->pushRange.size == 0u` → `XUD_INVALID_ARG`
  — a zero-size range names no bytes at all, and the stage flags of a zero-size range would be decoration, so the size is the
  field checked.

  **The row itself is a local of the loop iteration, and that is the one place this tree does NOT follow its own member rule**:
  every other row the driver reads is a member rather than a frame temporary because the driver reads the **pointer** later
  (`vkCmdBindDescriptorSets` keeps the set handles, `vkQueueSubmit` keeps the submit struct). `vkCmdPushConstants` takes
  `const void* data` and the driver **copies the bytes into the command buffer during the call**, so nothing above reads
  them after it and an iteration-local is legal. The comment beside the call says so, because the rule and the exception
  otherwise look identical in the diff.

  **The pure half**: `xud_frame_fill_push_row(outRow, drawRow)` — the model matrix is the **identity**, which is the same
  honest answer the block's `viewProj` gives and for the same reason (a non-identity matrix is a transform the camera/transf
  orm step has to CHOOSE, and that step is above this one and has not landed — see the *Later* item); what is not decoration
  is the channel, and the step above replaces the matrix without this rung's shape changing. `materialIndex` is the draw row's
  own with the batch's `XUD_BATCH_NO_MATERIAL` (0xFFFFFFFF) sentinel **mapped to slot 0**, and the reason is the argument's
  type: the push member is an index a read could use and 0xFFFFFFFF indexes nothing, while "no material slot yet" is a batch-
  side fact about a subsystem that does not exist. `textureIndex` is 0 because set 1 is declared and never allocated, so there
  is no sampler array with an entry in it. `flags` is the variant the draw was flushed under — the pipeline variant is chosen
  by BINDING a different pipeline, and this is the same fact from the shader's side, which is what lets a stage branch on the
  variant without a third pipeline. The vertex stage reads only the model matrix today, so the other three members are record
  ed-and-unread: the row is the channel and a stage need not touch every member of a block its layout declares.

  **The shader**: `shaders/solid.vert` gains `layout(push_constant) uniform XudPushRow { mat4 modelMatrix; uint materialInd
  ex; uint textureIndex; uint flags; uint reserved; }` and `gl_Position = xudFrame.viewProj * xudPush.modelMatrix * vec4(inP
  os, 1.0f)`. The member order/widths are the C layout of `XudPushRow` read against std140 — mat4 at 0 (64 B), uint at 64/68
  /72/76, 80 B total, byte-for-byte the struct in `xud_descriptor.h` and the assert beside `XUD_DESCRIPTOR_PUSH_SIZE` is the
  same number from the other side. Both matrices are identity on this machine today, so the drawn geometry does not move and
  no pixel expectation in any test changes — the fact this step adds is the multiplication and the two reads, not a new image.

  **Tests**: `tests/xud_test_frame.c`'s pure half gains the row in **both material shapes** (the sentinel and a real slot, bec
  ause the sentinel mapping is the branch this rung owns and the only one a synthetic row can exercise) and in **both varian
  ts**: the refusals (NULL out-row, NULL draw row — the row's variant and material slot are what the push carries, so there is
  nothing to carry without one), the identity's diagonal **and** its twelve off-diagonal zeros (so the matrix is the identity
  rather than a matrix that happens to have ones on its diagonal), the sentinel mapping to 0, texture 0 / flags = the variant /
  padding zeroed, and a real slot passed through unchanged with the variant following the row (the per-draw channel carries the
  draw's own facts, not this rung's). The GPU half gains `gpu-draw/push-range-recorded`: the layout's range is a recorded row
  rather than a discarded one, and it is the row the frame loop pushes against, so the channel has an extent, an offset and two
  stages behind it rather than a call naming constants of the loop's own.

  **Bar met, dated one-liner**: headless **2293 checks / 0 failures** (was 2283) and `XUD_GPU_TESTS=1` **2896 / 0** (was 2885),
  under gcc **and** under `-DCMAKE_C_COMPILER=clang` (both build trees rebuild the generated header — `ninja` regenerates
  `build/gen/solid_vert.spv`, `spirv-val` runs in the same rule, and `xud_solid_spv.h` is rewritten from it, so the shader's
  new push block is validated in CI rather than assumed to compile), validation-clean with `VK_INSTANCE_LAYERS` unset and forced
  to `VK_LAYER_KHRONOS_validation` (still exactly the harness's own synthetic `ERROR/validation` + `WARNING/validation` pair in
  either run and nothing else — the push call's stage flags/offset/size pass the layer's subset-and-extent rules against the
  layout the pipeline was created with, and the `Gtk-WARNING` lines GLFW's Wayland backend prints are not validation output),
  `-DXUD_SANITIZERS=ON` green headless (**72 B / 3 allocations**) and GPU **1336 B / 68** — byte-for-byte S18's numbers, so the
  push step added no allocation and no leak, and with `ASAN_OPTIONS=detect_leaks=0` answering 2293 / 2896 with 0 failures and no
  `runtime error` line; `./scripts/format.sh --check` green; `ar -t build/libxud.a` stays **40** objects (no new TU — the row is
  the frame rung's own pure half); `nm -u` lists no `vk*`/`glfw*` symbol and `ldd` lists no `libvulkan`/`libglfw`/`libGL`/`libEGL`
  for either executable (the four Wayland libs GLFW links PUBLIC are still the only window-system libs present).

  **Honest boundary**: the push row is live end to end (filled, pushed, read), but the matrix in it is the identity for the same
  reason the block's `viewProj` is — a non-identity matrix is a transform the camera/transform step has to choose, and that step
  is a *Later* item; the material / texture / flags members of the same row are recorded and unread, which is the channel the
  material, texture and variant steps above will fill rather than a decoration of this one (the variant member is the fact the
  transparent step below will branch on); and the frame loop still records **one** draw row per frame, so the push is exercised
  once per frame today and N times per frame the moment a mid-frame flush exists — `xud_batch_should_flush()` is still a decision
  no caller acts on.

## Steps complete (S18 The per-frame uniform update — a host-visible arena of its own block, the block written and copied every frame, `vkCmdBindDescriptorSets` for set 0 and the shader reading the set-0 `viewProj`, one step, landed as one commit, 2026-10-26)

- [x] ~~**Batch consumers (part 2 of 4)**: the per-frame uniform copy (the `viewProj` row `xud_descriptor_fill_uniform_block()`
    writes was touched by nothing today), **`vkCmdBindDescriptorSets`** for set 0, and the shader reading the **set-0
    `viewProj`**~~ — **done 2026-10-26**. What stays open in the item, and stays in `todo.md`: `vkCmdPushConstants` and the
    shader reading the **push-constant model matrix**, the **transparent** pipeline variant + the sorted-batch rule, and
    the handle layer's `record`.

  **The allocator gains a second acquire, and the reason one is not enough**: `xud_allocator_acquire_host_visible()`.
  `vkMapMemory` maps a **`VkDeviceMemory`**, not a range, so a suballocated carve inside a block someone else already
  mapped cannot be mapped at all — the validation layer states it as `VUID-vkMapMemory-memory-00678` and lavvk answers
  the same rule as `VK_ERROR_MEMORY_MAP_FAILED` rather than forgiving it (probed here: with the batch's arena suballocated
  out of a shared block, the map of it failed and the validation layer named that VUID; the same carve taken as a whole
  block maps clean). So the one carve a rung maps has to own the block it is mapped in, and this is the repo's "allocate
  once, suballocate always" rule read with its **one exception stated and the exception a Vulkan fact rather than a
  policy**: suballocation is for the bytes a frame reads through a transfer, a host mapping is a whole-memory decision.
  The new acquire shares every request-side guard and both decisions (the caps' memory type, this layer's block size)
  with `xud_allocator_acquire()`; the difference is that there is **no free-list scan at all** — reusing a range would be
  reusing a memory that may already carry a mapping, which is the refusal above — the range handed back is the **whole
  block**, and it is never split. `xud_allocator.h`'s doc comment says all of this beside the declaration.

  **Two arenas spend that exception, and both were already in the tree**: the batch's staging arena (`xud_batch_create_staging()`
  switched from `xud_allocator_acquire()` to the host-visible one — a change to the S16 rung, stated in its own comment
  there rather than left to the diff) and the descriptor rung's new arena for set 0's block. A completed context therefore
  owns **four** `VkDeviceMemory` and **nine** live ranges, not three and eight, and every block/range count in the
  descriptor, render-pass, core and allocator areas' GPU halves moved with it (the allocator area's numbers stay read as
  a **delta on the baseline** for that reason, which is what kept the one stale count from being a magic number).

  **The descriptor rung owns the arena, not the frame loop**: `XudDescriptor` gains `uniformStagingBuffer`/`uniformStagingInfo`/
  `uniformStagingRange`/`uniformStagingBindOffset`/`uniformStagingMapped`/`uniformStagingMappedLive` and the upload's one
  `VkBufferCopy` region, plus `XUD_DESCRIPTOR_STAGING_USAGE` (`TRANSFER_SRC` only — the arena is read by the copy and never
  by a shader stage, and the assert beside it is that its usage is **disjoint** from set 0's uniform buffer's, so a buffer
  that asked for both would say nothing about which side it was) and `XUD_DESCRIPTOR_UPLOAD_REGION_COUNT` **1** — one
  region carries the whole block, which is the opposite of the batch's five-call shape, where the five destinations are
  five buffers. Four new rungs: `xud_descriptor_host_visible_bits()` (the AND of every caps `memoryTypes` row carrying
  `VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT`, a **preference** mask handed to the allocator, same shape as `xud_batch_host_visible_bits()`
  one rung over — zero means the driver has no host-visible type at all and is reported, not worked around), `xud_descriptor_fill_staging_info()`
  (size = one block, no headroom, because the only thing ever written into it is one `XudFrameUniformRow`), `xud_descriptor_fill_upload_info()`
  (one region at (0, 0, the block's size) — both buffers are exactly one block long and the block starts at the start of
  each, and the extent is the block's size rather than either buffer's, which is the rule `vkCmdCopyBuffer` states: a region
  must fit inside **both**) and `xud_descriptor_write_block()` (the ONE host write: a `memcpy` of the block into the mapped
  arena, no driver call, so the block's layout stays testable with no device present up to the one guard it runs — an
  unmapped arena is a state this rung refuses rather than a write into NULL). `xud_descriptor_upload()` is the driver half
  of the copy: ONE `vkCmdCopyBuffer` carrying the one region, and it fills the rows itself rather than trusting the bring-up,
  which is what lets the headless run call the assembly on its own and the driver still get the rows it asserted.

  **The order, both ways, is one order**: the arena comes **last** in `xud_descriptor_open()` because it is the only object
  of the six that nothing else in this rung names, and putting it last is what makes the teardown below the exact reverse
  rather than a second order to state. `xud_descriptor_retire()` therefore **unmaps FIRST**, above every destroy — this is
  the one place a descriptor dies and the repo's rule is that a mapped range has a matching unmap; a destroy of a buffer
  whose memory is still mapped is a destroy over a mapping the driver still holds. `vkUnmapMemory` returns void, so the
  handle is read into a local, the live bit cleared, and only then handed over (the zero-state-before-the-call rule every
  void row in this tree follows). The caps gate that can now refuse the bring-up is `XUD_UNSUPPORTED` for "no host-visible
  memory type exists to bind the arena to", and the chosen type is re-checked for the bit **after** `xud_allocator_select_memory_type()`
  rather than trusted from the mask.

  **The frame loop** (`xud_frame_draw` gains the descriptor argument after the batch, and `XudRenderer` holds a borrowed
  pointer to the context's descriptor for the same outlive rule the batch's note gives — the pipeline below was built
  against **this** record's two set layouts and the loop binds **this** record's set 0): set 0's block is uploaded **every
  frame, whether or not the batch has content**, because the pipeline's fragment stage reads this block and a frame that
  never uploaded it is a frame whose shader reads a buffer that was never written. The write and the copy are the descriptor
  rung's; what belongs to this rung is the barriers and the bind. ONE `HOST_WRITE → TRANSFER_READ` barrier now covers
  **both** arenas' host writes — a barrier is a scope, not a buffer — which is why the batch's own copy block no longer
  records a second one, and the block's copy sits after the has-content block rather than inside it because the block
  **always** has content. The barrier that makes that copy's result visible is a **second** `TRANSFER_WRITE` barrier, and
  the split is load-bearing: the spec's supported-access-type rule binds an access bit to the stages that can perform it,
  `VK_ACCESS_UNIFORM_READ_BIT` belongs at the two shader stages and `VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT` at the input and
  vertex stages, and one barrier naming both access bits with one stage mask naming all three is refused. The new macros
  `XUD_FRAME_UNIFORM_READ_STAGES`/`XUD_FRAME_UNIFORM_READ_ACCESS` are that pair, and the stages are `XUD_DESCRIPTOR_BUFFER_STAGES`
  seen from the other side of the ladder. `vkCmdBindDescriptorSets` is recorded beside the other two binds, outside the
  render pass, against **`pipeline->layout`** — the layout the pipeline was created with is the layout the set was allocated
  against, so naming it rather than a layout of this rung's is what makes the two agree rather than two records that happen
  to match — `firstSet` 0, count 1 (`XUD_FRAME_FIRST_SET`/`XUD_FRAME_SET_COUNT`), set 1 staying the texture step's.

  **The shader**: `shaders/solid.vert` declares `layout(binding = 0, std140) uniform XudFrameBlock` and `gl_Position` is
  now `xudFrame.viewProj * vec4(inPos, 1.0f)`. The member order and types are the C layout of `XudFrameUniformRow` read
  against std140 — `mat4` at 0 (64 B), `vec3` at 64, `float` at 76, `vec4` at 80 and 96 (both already 16-aligned), `uint`
  at 112 and 116 — which is byte-for-byte the struct in `xud_descriptor.h`, and the assert beside that struct is the same
  120 from the other side. The push-constant model matrix is still unread and the layout's push range still declared-but-unread.

  **Tests**: `tests/xud_test_descriptor.c`'s pure half gains the arena's create-info (stamped, one block long, `TRANSFER_SRC`
  only, and disjoint from the buffer's usage), the upload's one region (both offsets 0, extent the block's size — assembled
  against the synthetic pair of stand-in handles, which is what makes the GPU run's copy a check against a row the headless
  run asserted), the host write's refusal (`XUD_INVALID_ARG` on NULL, `XUD_UNSUPPORTED` with no arena mapped) and the host-visible
  preference read off a synthetic caps in all three shapes (the mask names only the host-visible row, a caps with no such row
  gives an **empty mask** rather than a failure — the refusal belongs where the arena is acquired, because an empty mask
  narrows the choice to nothing). Its GPU half reads the arena back **after** the bring-up: live over its own carve, the
  carve is the whole block, mapped exactly once for the rung's life, and `xud_descriptor_write_block()` really put the block's
  bytes in it — the read-back is the copy's source bytes, which is what makes the upload's one region a fact rather than an
  intent. `tests/xud_test_frame.c` gains the set-and-block liveness and the arena's mapped state **before** the first frame,
  the new `xud_frame_draw` refusal (no descriptor is a bad argument, not a draw of a pipeline whose block was never written),
  the block's identity before the loop, and **after** the loop that the frames unmapped nothing — the arena is the one mapping
  the descriptor's own close will unmap, and a per-frame map/unmap pair would have left the flag clear. `tests/xud_test_allocator.c`
  gains the new acquire's headless guards (the same request-side refusals as the suballocating one, and the same unbound-loader
  answer, "a block it creates would be a block it cannot free") and its GPU half the acquire against the **real** caps: the
  range is a whole block, starts at 0, the block count moves by exactly one and the reserved bytes move by the whole block
  rather than by the 2,048 that were asked for — the cost of the exception stated as a fact. `tests/xud_test_core.c` and
  `tests/xud_test_renderpass.c` move their block/range counts with the rest.

  **Bar met, dated one-liner**: headless **2283 checks / 0 failures** (was 2263) and `XUD_GPU_TESTS=1` **2885 / 0** (was
  2848), under gcc **and** under `-DCMAKE_C_COMPILER=clang`, validation-clean with `VK_INSTANCE_LAYERS` unset and forced
  to `VK_LAYER_KHRONOS_validation` (still exactly the diagnostics layer's own synthetic `ERROR/validation` + `WARNING/validation`
  pair in either run and nothing else — the map/unmap order, the two-barrier split and the bind all pass the layer, and the
  `VUID-vkMapMemory-memory-00678` refusal the new acquire exists against is **not** raised by any of them), `-DXUD_SANITIZERS=ON`
  green headless (**72 B / 3 allocations**, byte-for-byte S16's and S17's numbers) and GPU **1336 B / 68** — reporting **only**
  the external allocations the S7–S17 blocks record, and the two stacks that name an xud source are the same 32 B inside
  `vkCreateDevice` (`xud_device.c:374`, the validation layer's) and the same 8 B in `libdecor_new` (`xud_window.c:156`, GLFW's)
  every other area reports; no new leak stack names `xud_descriptor_*`/`xud_allocator_*`/`xud_frame_*`, and the arena's block
  is freed by `xud_allocator_close()` under ASan; with `ASAN_OPTIONS=detect_leaks=0` answering 2283 / 2885 with 0 failures and
  no `runtime error` line; `./scripts/format.sh --check` is green over the tree; `ar -t build/libxud.a` stays **40** objects
  (no new TU this step — the arena is the descriptor rung's own half and the allocator's own half); `nm -u` lists no `vk*`/`glfw*`
  symbol and `ldd` lists no `libvulkan`/`libglfw`/`libGL`/`libEGL` for either executable (the four Wayland libs GLFW links PUBLIC
  are still the only window-system libs present).

  **Honest boundary**: the per-frame update is live end to end — the block is written, copied, bound and read — but the block's
  contents are still the identity `xud_descriptor_fill_uniform_block()` writes, because a camera that is not identity is the
  camera/transform step's to choose; the push-constant range of the same layout is still declared-and-unread, and pushing a
  range a stage does not read is a call that changes nothing about what is drawn, so it stays the next step's; the transient
  **ring** still has no caller outside the allocator area's own test, and the per-frame uniform copy is **not** what wanted
  it — the arena is a static carve because a ring carve is bytes a later lap overwrites mid-frame, and these bytes have to
  survive from the moment the host wrote them to the moment the copy read them.

## Steps complete (S17 The batch's consumers — the pipeline's vertex input names the batch's four streams, the frame loop uploads / binds / draws them, the indexed multi-draw replay and the per-frame reset, one step, landed as one commit, 2026-10-26)

- [x] ~~**Batch consumers (part 1 of 2)**: the pipeline's vertex-input state, the frame loop's indexed draw and its
    two command-buffer bindings (`vkCmdBindVertexBuffers` over the four streams, `vkCmdBindIndexBuffer` over the index
    buffer), the upload (`xud_batch_upload()` called from the frame loop), the frame loop's multi-draw shape and its
    per-frame batch reset~~ — **done 2026-10-26**. What stays open in the item, and stays in `todo.md`: `vkCmdBindDescriptorSets`
    for set 0, `vkCmdPushConstants`, the per-frame uniform copy (the `viewProj` row is written by nothing today), the
    **transparent** pipeline variant + the sorted-batch rule, and the handle layer's `record`.

  **The shader**: `shaders/solid.vert` no longer generates three vertices from `gl_VertexIndex`. It declares the four
  inputs at locations 0..3 (`inPos` `vec3`, `inUv` `vec2`, `inColor` `vec4`, `inNormal` `vec3`) — the locations `xud_batch_fill_attribute`
  writes and `xud_pipeline_fill_vertex_input` hands the pipeline — and outputs `outUv`/`outColor`/`outNormal` at 0/1/2.
  `gl_Position = vec4(inPos, 1.0f)` because the push-constant model matrix and the set-0 `viewProj` are still unread
  (the frame loop has to bind both before a stage can read them, and that binding is the next step's half); a shader
  need not touch everything its layout declares, so the push range and the two set layouts stay declared-but-unread,
  which is legal rather than half-built. `shaders/solid.frag`'s `inColor` moved from `location = 0` to `location = 1`
  because the vertex stage's `outColor` is at 1 (0 is `outUv`, a `vec2`) — the validation layer's
  `VUID-RuntimeSpirv-OpEntryPoint-08743` refuses a frag input whose Location/Component slots the previous stage does
  not output, and it reported that mismatch as a hard error on this machine before the fix, in the **headless** run too
  (the ungated `xud_test_init_bringup_boundary` completes a real context here, so a pipeline really is built with no
  `XUD_GPU_TESTS` set).

  **The pipeline**: `xud_pipeline_fill_vertex_input(pipeline, batch)` is the pure overwrite — it refuses a NULL pipeline/batch
  and a batch whose four binding numbers and four attribute locations are not the stream numbers 0..3 (a row still all-zero
  never went through `xud_batch_open`'s fill, and a pipeline built on it would read a binding number that names no stream),
  then points `pVertexBindingDescriptions`/`pVertexAttributeDescriptions` at the **batch's own arrays** with both counts
  `XUD_BATCH_STREAM_COUNT`. Pointing rather than copying is legal because the renderer's open/close order opens the batch
  before the pipeline and closes it after, so the pointer cannot outlive what it names. `xud_pipeline_open` takes the batch
  and calls the overwrite after `xud_pipeline_fill_state`, which is why `fill_state` still writes the zero row: that row is
  the **starting state** of the field, and keeping it is what lets the headless assembly test ask "is the row stamped, is
  its `pNext` clear" with no batch in hand.

  **The frame loop** (`xud_frame_draw`, batch argument added after the framebuffer array): the batch is flushed **before**
  the command buffer is begun — `xud_batch_stats()` then, on content, `xud_batch_record_draw(batch, batch->currentVariant,
  batch->currentMaterial)` closing the pending run and a re-read of the stats — because closing a row and reading the row
  list are pure host work, and doing them above the begin call is what keeps every failure path below it one that ends the
  buffer it began rather than returning out of a begun one. `xud_batch_record_draw` returns `XUD_OUT_OF_MEMORY` rather than
  a code of its own because the row array is grown by `XUD_BATCH_DRAW_MIN` and a refusal there is an allocation this rung
  cannot make. Then, outside any render pass instance and before `vkCmdBeginRenderPass`: a `HOST_WRITE → TRANSFER_READ`
  `vkCmdPipelineBarrier` (the arena is mapped **once** for the batch's whole life by `xud_batch_open`/`xud_batch_close`, so
  these are not a map/unmap pair per frame and the repo's matching-unmap rule does not reach them — the barrier is what
  makes the writes visible, and `HOST` is a legal `srcStageMask` and never a `dstStageMask`, which is why this is its own
  call rather than a third entry folded into the layout-transition barrier above), then `xud_batch_upload()`'s five
  `vkCmdCopyBuffer` calls over the **live** byte counts rather than the capacity (the arena's bytes are rewritten in place
  every frame, never appended to, so the rest is stale bytes no draw reads), then a `TRANSFER_WRITE → VERTEX_ATTRIBUTE_READ`
  barrier, then **one** `vkCmdBindVertexBuffers` call with `firstBinding 0` and `bindingCount XUD_BATCH_STREAM_COUNT` over
  a stack array of the four stream `VkBuffer`s at offset 0 each (four separate calls would leave three of the pipeline's
  four bindings unbound, and the offset is **into the buffer**, not the buffer's address in the allocator's block — each
  stream is its own `VkBuffer` bound at its range's offset, so the vertex data starts at 0), then one `vkCmdBindIndexBuffer`
  call at offset 0 with `VK_INDEX_TYPE_UINT32` (`XUD_BATCH_INDEX_SIZE` is 4 bytes an entry, so `UINT16` would be a bind
  reading half an index and `UINT8` a bind reading a quarter of one). Inside the pass: one `vkCmdDrawIndexed` per recorded
  row. **This machine's `vulkan-headers 1.4.363` declares `vkCmdDrawIndexed` without an index-buffer argument** —
  `(commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset /* int32_t */, firstInstance)` — and declares
  `vkCmdBindIndexBuffer` beside it, so the index buffer is bound as state and the draw reads it; the `static_assert` pair
  on the two `PFN` rows and the loader's nil-row checks are what pin that reading rather than a memory of the spec. A row
  that the accessor cannot read **ends** the list rather than returning out of a begun pass (the count came from the same
  batch, so the branch is not one the loop reaches, and the draws made so far are the whole list either way, which is what
  keeps the command buffer well formed). An empty batch is a legal clear-and-present frame: no copies, no binds, no draws.
  And `xud_batch_reset(batch)` runs at the end of a frame that **completed** — capacities and buffers stay, only the live
  counts restart — while a frame that failed above leaves the batch alone, because a batch whose content was never uploaded
  is a batch a caller above still has to see.

  **The renderer** passes the batch to both: `xud_pipeline_open(..., renderer->batch, renderPass)` and
  `xud_frame_draw(..., framebufferCount, renderer->batch, outImageIndex)`. The open/close order is unchanged and is still
  load-bearing: open pass → batch → pipeline → framebuffer array → frame, close in exact reverse.

  **The tests**: `tests/xud_test_pipeline.c` grew the headless half of the overwrite — a synthetic batch whose four binding
  numbers, four strides, four locations and four formats are filled by hand (filling by hand rather than by `xud_batch_open`
  because open creates six buffers and needs a device, and the row **values** are all the overwrite reads), checked against
  the OK path, the two NULL refusals, the zero-row refusal and "a refused overwrite leaves the row untouched". Its two
  `zero-bindings`/`zero-attributes` checks stay (they are checks of the row `fill_state` writes) with their reasons
  rewritten to say so. `tests/xud_test_frame.c`'s GPU half now appends a **real triangle** before every frame — three
  fixed positions inside the clip volume, fixed uv, fixed white colour, fixed +Z normal, so the run is deterministic and
  the validation layer sees the same geometry every run — which is what makes the upload / bind / draw path inside
  `xud_frame_draw` the path this run actually takes rather than the empty-batch path a green run would say nothing about.
  It adds the `NULL`-batch refusal, the batch-present / index-buffer-live / `xud_batch_check_invariant` reads before the
  first frame, and inside the loop the two checks that the frame reset the batch (`vertexCount == 0u` and `drawCount == 0u`
  after every drawn frame, so the triangle that was drawn is gone before the next frame appends its own). `tests/xud_test_batch.c`'s
  file-top note that the copies are "not exercised by a GPU test yet" is replaced with the truth: the three bindings and the
  five copies are exercised by the frame area's GPU half.

  **Bar met, dated one-liner**: headless **2263 checks / 0 failures** (was 2255) and `XUD_GPU_TESTS=1` **2848 / 0**
  (was 2804), under gcc **and** under `-DCMAKE_C_COMPILER=clang`, validation-clean with `VK_INSTANCE_LAYERS` unset and
  forced to `VK_LAYER_KHRONOS_validation` (still exactly the diagnostics layer's own synthetic `ERROR/validation` +
  `WARNING/validation` pair in either run and nothing else — the pipeline's SPIR-V-interface error the new shader pair
  would otherwise raise is gone), `-DXUD_SANITIZERS=ON` green headless (**72 B / 3 allocations**, byte-for-byte S16's
  numbers) and GPU **1432 B / 69** (byte-for-byte S16's numbers), reporting **only** the external allocations the S7–S16
  blocks record — no leak stack names `xud_batch_*`/`xud_renderer_*`/`xud_frame_*`/`xud_pipeline_*`, and the two stacks
  that name an xud source are the same 32 B inside `vkCreateDevice` (`xud_device.c:374`, the validation layer's) and the
  same 8 B in `libdecor_new` (`xud_window.c:156`, GLFW's) every other area reports; with `ASAN_OPTIONS=detect_leaks=0`
  answering 2263 / 2848 with 0 failures and no `runtime error` line; `nm -u` lists no `vk*`/`glfw*` symbol and `ldd`
  lists no `libvulkan`/`libglfw`/`libGL`/`libEGL` for either executable; `./scripts/format.sh --check` is green over the
  tree; `ar -t build/libxud.a` stays **40** objects (no new TU this step — the batch's consumers are the pipeline and
  frame rungs' own halves).


## Steps complete (S16 The batch rung — four SoA stream buffers + one index buffer + one mapped staging arena, the flush decision and the capacity arithmetic, one step, landed as one commit, 2026-10-26)

- [x] ~~**Batch layer (`xudrl`)**: dynamic per-attribute **stream** buffers + one index buffer (SoA, decided 2026-10-23 — see
    Decided below), flush on pipeline/material change (flush is per stream), fixed pipeline set~~ — **done 2026-10-26, the
    rung itself**: `src/xud/xud_batch.h`/`.c` and `tests/xud_test_batch.c` are in the tree, the renderer owns one `XudBatch*`
    per context, and the headless half of the rung is green. What is **not** in this step, and stays open in `todo.md`: the
    pipeline's vertex-input state (still zero bindings/zero attributes), the three command-buffer bindings
    (`vkCmdBindVertexBuffers`/`vkCmdBindDescriptorSets`/`vkCmdPushConstants`), the indexed draw (`xud_frame.c` still records
    `vkCmdDraw`), the per-frame uniform copy, the **transparent** pipeline variant and the sorted-batch rule, the handle
    layer's `record`, and the frame loop's multi-draw shape.

  **The rung**: `XudBatch` owns six `VkBuffer`s — four stream buffers (`pos` `R32G32B32_SFLOAT` 12 B, `uv` `R32G32_SFLOAT`
  8 B, `color` `R8G8B8A8_UNORM` 4 B, `normal` `R32G32B32_SFLOAT` 12 B = the settled 36 B/vertex **split across four buffers
  rather than interleaved into one**) and one index buffer (`uint32_t`, `XUD_BATCH_INDEX_SIZE 4u`, the one width that never
  needs a second index buffer past 65535 vertices) — plus one host-visible staging arena that all six are carved out of. The
  arena is one buffer holding the four streams as fixed windows plus an index window, so the whole frame's attribute bytes ar
  e **one** host allocation and **one** map, and the copy is five `vkCmdCopyBuffer` calls (five destinations is five calls, n
  ot one call with five regions — `vkCmdCopyBuffer` names one source and one destination buffer).

  **The pure half, headless by construction**: `xud_batch_layout()` (the four stream offsets, the index offset, the arena tot
  al and the index capacity — `XUD_BATCH_INDEX_CAPACITY(v) = (3v+1)/2`, Euler's 3n/2 bound rounded up, not a guess), `xud_bat
  ch_next_capacity()` (the doubling and its bound), `xud_batch_fill_binding()`/`xud_batch_fill_attribute()`/`xud_batch_fill_v
  ertex_input()` (the two vertex-input rows per stream, gated on the caps' `maxVertexInputBindings`/`maxVertexInputAttributes
  ` — 33/29 on this driver against this rung's 4, so headroom is read rather than assumed), `xud_batch_fill_copy_regions()`,
  `xud_batch_check_invariant()` (the settled SoA invariant, "every stream holds the same vertex count"), `xud_batch_should_f
  lush()` (flush on variant change, material change, or an append that would pass the capacity — the flush decision with th
  e driver calls taken out of it), `xud_batch_record_draw()` (the draw rows the frame loop will replay, grown by `XUD_BATCH_D
  RAW_MIN` rather than refused at a caller-visible bound), `xud_batch_append_vertex()`/`xud_batch_append_index()` (the host w
  rites into the mapped arena, refused rather than written past a buffer), `xud_batch_reset()`, `xud_batch_stats()` and `xud_
  batch_host_visible_bits()` (the mask of the caps' host-visible memory types, which is the one requirement the arena's allo
  cator request carries).

  **The arena is static, not transient, and that is the load-bearing decision of this step**: the staging arena is carved wi
  th `xud_allocator_acquire()` out of the allocator's **blocks**, not with `xud_allocator_ring_acquire()` out of its ring. Th
  e ring is a cursor that **wraps**, so anything it handed out at one lap is overwritten at a later one — and this arena hold
  s the frame's attribute bytes from the moment the appends wrote them until the moment the copies read them, so a ring carv
  e is a batch whose bytes a later lap of the ring overwrites mid-frame. Mapping it **once at open and once at close** is th
  e repo's "no `vkMapMemory` per frame without a matching `vkUnmapMemory`" rule satisfied without a per-frame pair to keep h
  onest; the per-frame part of the arena is its content being rewritten, not its mapping being redone. Consequence read as a
  fact in three GPU areas: the allocator's ring block **still does not exist** after a completed context, because nothing in
  this rung asks the ring for a byte — the frame loop will, one rung up.

  **The baselines that moved, and why**: the allocator's block count went **2 → 3** and its live-range count **2 → 8** at a
  completed context (the batch's arena, its four streams and its index buffer are six more carves, and they need one more bl
  ock only once the existing holes run out), which is the suballocation half of "allocate once, suballocate always" seen fro
  m above — six requests sharing blocks rather than six `vkAllocateMemory`. The GPU areas of the allocator, descriptor, ren
  derpass and core tests had their baselines restated to that truth, and two of the allocator area's own checks stopped bei
  ng absolute: `small1.blockIndex == 0` became `small1.blockIndex < baseBlocks` ("the request came out of a block that ALRE
  ADY existed", which is the stronger fact — which hole the scan picks is the free list's row order, not the block count's)
  , and `reuse.blockIndex == 0` became `reuse.blockIndex == small1.blockIndex` ("back in the SAME block the first request us
  ed", the coalesced free range rather than a fresh block). The ring's own offsets (0 / 256 / 1280, wrap back to 0) and its
  cursor (0 before, 256 after a reset lap) are **unchanged**, which is the static-arena decision read as a fact: had the ar
  ena come from the ring, every one of those numbers would have moved.

  **Ownership**: the renderer owns the batch, not `XudContext` — `struct XudRenderer` grew `XudBatch* batch`, opened after t
  he render pass and **before** the pipeline (the pipeline's vertex-input state is the thing that will name these buffers, s
  o they must exist before it and outlive it) and closed in the reverse slot, after the pipeline has been retired and befor
  e the render pass's teardown. `xud_renderer_retire()` grew the `xud_batch_close()` rung; the renderer's teardown grew the
  `free(victim->batch)` slot. `ar -t build/libxud.a` went **39 → 40** objects, the one new one being `xud_batch.c`.

  **Bar met, dated one-liner**: headless **2255 checks / 0 failures** (was 2147) and `XUD_GPU_TESTS=1` **2804 / 0** (was 267
  5), under gcc **and** under `-DCMAKE_C_COMPILER=clang`, validation-clean with `VK_INSTANCE_LAYERS` unset and forced to `VK
  _LAYER_KHRONOS_validation` (still exactly the diagnostics layer's own synthetic `ERROR/validation` + `WARNING/validation`
  pair in either run, nothing else), `-DXUD_SANITIZERS=ON` green headless (**72 B / 3 allocations**, byte-for-byte S15's nu
  mbers, so the batch's pure half allocates nothing) and GPU **1432 B / 69** (was 1296 B / 66) reporting **only** the extern
  al allocations the S7–S15 blocks record — no leak stack names `xud_batch_*`/`xud_renderer_*`/`xud_allocator_*`/`xud_frame
  _*`/`xud_pipeline_*`, and the two stacks that name `xud_test_batch_gpu` are the same 32 B inside `vkCreateDevice` (`xud_de
  vice_open`) and the same 8 B in `libdecor_new` (`xud_window_open`) every other GPU area reports, reached through the batch
  area's own `xud_init_context()` call — and with `ASAN_OPTIONS=detect_leaks=0` answering 2255 / 2804 with 0 failures and n
  o `runtime error` line; `nm -u` lists no `vk*`/`glfw*` symbol and `ldd` lists no `libvulkan`/`libglfw`/`libGL`/`libEGL` fo
  r either executable, and `./scripts/format.sh --check` is green over the tree.


## Steps complete (S15 The depth attachment + the opaque pipeline variant — the first `VkImage` in the tree, its own
`VkDeviceMemory` carve and view, the pipeline's depth pair ON against it, one step, landed as one commit, 2026-10-25)

- [x] ~~**Pipeline variants + depth attachment**: the opaque/transparent pair (blend off + depth write vs blend on +
  depth test, sorted) and the depth attachment the render pass does not have yet — a `VkImage` + `VkDeviceMemory` of
  its own, i.e. the first thing the allocator's rules exist for~~ — **done 2026-10-25, half of it**: the depth
  attachment and the **opaque** variant landed, the transparent variant did not, and that split is the point — the
  transparent half is blend on + depth test **sorted**, and sorting is the batch's rule, so the second pipeline is the
  batch step's to write, not this one's. `xud_renderpass.h` grows the second attachment row
  (`XUD_RENDERPASS_DEPTH_ATTACHMENT`, `XUD_RENDERPASS_ATTACHMENT_COUNT` 2), the pass's own `VkImage` + `VkImageView` +
  their create-infos + the `XudMemoryRange` the allocator handed the image, and the `VkAttachmentReference` row the
  subpass's `pDepthStencilAttachment` points at; `xud_renderpass.c` grows the pure half
  (`xud_renderpass_fill_depth_info`, `xud_renderpass_fill_depth_view_info`) and the driver half that creates the image
  out of an `xud_allocator_acquire()` carve, creates the one view of it, and retires in one order
  (`xud_renderpass_retire()`: pass → view → image → allocator release, every destroy handle read into a local and its
  field cleared **before** the call). `xud_pipeline_fill_state()`'s depth pair went `VK_FALSE`/`VK_FALSE` →
  `VK_TRUE`/`VK_TRUE` against that attachment — the ON pair is honest only because the pass under the pipeline has a
  depth buffer, which is exactly what S11's note said and why it wrote the OFF pair. `xud_frame.h`/`.c` grow one
  `VkImageLayout depthLayout` record and a `VkImageMemoryBarrier` **array** so the frame transitions both images per
  frame, the depth barrier copied from the colour one and then overridden in four places (access masks, `oldLayout`,
  `newLayout`, `image`/`subresourceRange`), and `xud_frame_draw()` refuses a pass with no depth image rather than
  recording a barrier against `VK_NULL_HANDLE`. `xud_allocator.h`'s `XUD_ALLOCATOR_ALIGN_MAX` went 256 → **65536**:
  that bound was a **buffer** fact and the depth image's `VkMemoryRequirements` answer `alignment 65536` here, so the
  ceiling had to move with the fact rather than refuse the image. `xud_core.c` needed no ordering change — the pass was
  already above the allocator in both the bring-up ladder and the teardown order. `tests/`: the pipeline area's
  `depth-pair-off` check became `depth-pair-on`, the renderpass area was rewritten for two attachment rows (its own
  depth image/view helpers, the dependency pair's stage/access masks, the GPU half asserting the live image, its carve,
  its view's `VK_IMAGE_ASPECT_DEPTH_BIT` range and both layouts), and the allocator/descriptor/core GPU areas'
  baselines moved from one block/one live range to **two**, because the depth image is the allocator's second block,
  not a suballocation of the first.

  **Bar met, dated one-liner**: headless **2147 checks / 0 failures** (was 2108) and `XUD_GPU_TESTS=1` **2675 / 0**
  (was 2588), under gcc **and** under `-DCMAKE_C_COMPILER=clang`, validation-clean with `VK_INSTANCE_LAYERS` unset and
  forced to `VK_LAYER_KHRONOS_validation` (still exactly one `ERROR/validation` line in either run, the diagnostics
  layer's own synthetic headless probe), `-DXUD_SANITIZERS=ON` green headless (**72 B / 3 allocations**, byte-for-byte
  S14's numbers, so the depth attachment's pure half allocates nothing) and GPU **1296 B / 66** (was 1352 B / 65)
  reporting **only** the external allocations the S7-S14 blocks record — no leak stack names
  `xud_renderpass_*`/`xud_frame_*`/`xud_allocator_*`/`xud_pipeline_*`, every one of them is the libdecor 8 B, a
  validation-layer 32 B inside `vkCreateDevice`, or the `wl_display_*` compositor traffic inside
  `libwayland-client.so.0` — and with `ASAN_OPTIONS=detect_leaks=0` answering 2147 / 2675 with 0 failures and no
  `runtime error` line; `ar -t build/libxud.a` still lists **39** objects (no module added, two grown), `nm -u` lists
  no `vk*`/`glfw*` symbol and `ldd` lists no `libvulkan`/`libglfw`/`libGL`/`libEGL` for either executable, and
  `./scripts/format.sh --check` is green over the tree.

## Steps complete (S14 The test split — `tests/` from one 5092-line file to a harness, a runner and 17 area TUs, one step, landed as one commit, 2026-10-25)

- [x] ~~**Test split**: `tests/xud_test.c` is 5092 lines and one file — split it into one TU per area, with a
  shared harness and a runner, so an area can be read and built on its own~~ — **done 2026-10-25**: `tests/` is
  **19 files** now. `tests/xud_test_harness.h` declares the two counters as `extern`, the one named check,
  the GPU gate and the 17 area entry points; `tests/xud_test_harness.c` **defines** `xudChecks`/`xudFailures`
  once and keeps `xud_check()` and `xud_gpu_tests_enabled()` byte-for-byte as they were (the gate is still the
  compile-time `XUD_GPU_TESTS` default from `xud.h` overridden by the `XUD_ENV_GPU_TESTS` environment name);
  `tests/xud_test_main.c` is the runner — one call per area, in layer-ladder order, returning `xudFailures == 0`.
  The 17 areas are `xud_test_vocabulary`/`loader`/`diagnostics`/`instance`/`physical`/`device`/`gpu_bringup`/`window`
  /`swapchain`/`handle`/`shader`/`pipeline`/`renderpass`/`frame`/`allocator`/`descriptor`/`core`, the GPU bring-up
  area keeping `xud_test_gpu_instance()` → `xud_test_gpu_physical()` → `xud_test_device_gpu()` **nested in one TU**
  because that nesting is the ownership proof (a `VkDevice` is created from a `VkPhysicalDevice` handle and a
  `VkPhysicalDevice` is a query result of an instance, so each teardown must run while its owner is alive) and
  passing those handles through globals would be a second owner of the same state. `xud_test_handle()` is both
  the area entry point and that area's only test function. `CMakeLists.txt` names the 19 sources **explicitly, in
  that order** — no glob — so the area order is readable where it is set. **No file under `src/xud/` was touched
  and no check was added, changed or removed**: the archive is still 39 objects and the counts are still 2108
  headless / 2588 GPU. See AGENTS.md's **Verified test split (2026-10-25)** block for the ground (the harness
  ownership, the runner order, the per-area include rule and the two areas that needed a header beyond their own
  rung — `xud_core.h` for the frame area because `xud.h` only forward-declares `XudContext`, `xud_pipeline.h`
  for the renderpass area because `XUD_PIPELINE_SAMPLE_COUNT` is the pipeline layer's constant — the one real
  uninitialized-record bug the split exposed and the clang build caught, and the unchanged counts/linkage under
  gcc, clang, the validation layer and the sanitizers).

  **Bar met, dated one-liner**: headless **2108 checks / 0 failures** (unchanged) and `XUD_GPU_TESTS=1` **2588 / 0**
  (unchanged), under gcc **and** under `-DCMAKE_C_COMPILER=clang` after one fix the clang build demanded —
  `XudSurfaceCaps queried` in `xud_test_swapchain_pure()` was never zeroed while
  `swapchain/query-refusal-clears-caps` asserts `queried.surfaceFormatCount == 0u` after a **refused** query, so
  that check was reading uninitialized memory: gcc's stack happened to be zero and the single-file run passed,
  clang 22 answered **2588 / 1** and the FAIL was exactly that check, fixed by `memset(&queried, 0, sizeof queried)`.
  Validation-clean with `VK_INSTANCE_LAYERS` unset **and** forced to `VK_LAYER_KHRONOS_validation` (exactly one
  `ERROR/validation` line in either run, still the diagnostics layer's own synthetic headless probe), `-DXUD_SANITIZERS=ON`
  reporting **only** the external allocations the S7–S13 blocks record — headless **72 B / 3 allocations**, byte-for-byte
  S13's numbers, so the split moved no allocation, and GPU **1256 B / 64** (S13's 1352 B / 65 minus one 96 B
  `wl_display_roundtrip_queue` allocation inside `libwayland-client.so.0`, the compositor traffic the present step
  generates, which is variable run to run and nothing in xudlibc allocates, frees or can see), with
  `ASAN_OPTIONS=detect_leaks=0` answering **2108 / 2588 with 0 failures and no `runtime error` line**; `ar -t build/libxud.a`
  still lists **39** objects (the split adds no library object — the 19 test files are sources of the `xud_test`
  executable), `nm -u build/xud_test` still lists no `vk*` and no `glfw*` symbol and `ldd` still lists no
  `libvulkan`, no `libglfw` and no `libGL`/`libEGL`; `./scripts/format.sh --check` is green over the whole tree
  after formatting the new files with the existing `./scripts/format.sh` (it already covers `tests`).

## Steps complete (S13 The descriptor-layout rung — two set layouts, a pool, set 0, a uniform buffer bound to the allocator's first block, a push-constant range, one step, landed as one commit, 2026-10-25)

- [x] ~~**Descriptor layout**: set 0 per-frame uniforms, set 1 sampler array, push constants for model matrix +
  material/texture index — the rung above the allocator and below the batch~~ — **done 2026-10-25**: `src/xud/xud_descriptor.h`/`xud_descriptor.c`
  land **above the allocator and below the renderer**, `xud_core.h` grows a typed `XudDescriptor*` field, `xud_init_context()`
  opens it **after the allocator and before the renderer** and `xud_close_context()` closes it **after the renderer and
  before the allocator** — the order is load-bearing because a `VkBuffer` is created from memory the allocator owns and
  is named by the pipeline layout the renderer builds, so the order in is allocator → descriptor → renderer and the order
  out is the exact reverse. `xud_renderer_open()` and `xud_pipeline_open()` both take the `const XudDescriptor*` and
  **borrow** it: the renderer draws against these sets, it does not own them. `xud_pipeline_fill_layout_info()` grows from
  zero sets and zero ranges to `setLayoutCount = XUD_DESCRIPTOR_SET_COUNT` and `pushConstantRangeCount = 1u` read off
  that record (and it refuses a record whose two set layouts or uniform buffer are not live, so the layout and the handles
  cannot disagree), and the CMake source list grows the new `.c`. See AGENTS.md's **Verified descriptor-layout facts
  (2026-10-25)** block for the ground (the stamps, the `VkDescriptorPoolSize.descriptorCount` and
  `VkWriteDescriptorSet.pBufferInfo` naming traps that bit the build, the two `void` rows and their zero-state rule,
  the caps gate and why a refusal is not a shrink, the sampler array as a layout ceiling rather than a caps read,
  set 1's layout-without-a-set split and the `VK_EXT_descriptor_indexing` reason the partially-bound form stays a gate,
  the zero push-constant range the validation layer refuses and the driver accepts silently, the std140 byte layouts,
  and the teardown order with the no-`vkDestroyDescriptorSet` reason behind the pool-first step).

  **Bar met, dated one-liner**: headless **2108 checks / 0 failures** (was 2020), `XUD_GPU_TESTS=1` **2588 / 0** (was
  2460) and validation-clean with `VK_INSTANCE_LAYERS` unset **and** forced to `VK_LAYER_KHRONOS_validation` (the only
  `ERROR/validation` line in either run is still the diagnostics layer's own synthetic headless probe), `-DXUD_SANITIZERS=ON`
  reporting **only** the external allocations the S7/S8/S9/S10/S11/S12 blocks record — headless **72 B / 3 allocations**,
  byte-for-byte the S12 step's numbers, so the descriptor's pure half leaks nothing, and GPU **1352 B / 65** (was 1312 B
  / 63: one more libdecor 8 B and one more validation-layer 32 B whose four frames between `malloc` and `xud_device_open`
  are `<unknown module>`, i.e. inside `vkCreateDevice` and not inside lavvk and not inside xudlibc — the GPU descriptor
  test opens one more context), `ASAN_OPTIONS=detect_leaks=0` answering 2108 / 2588 with 0 failures and no `runtime error`
  line, the clang build (`-DCMAKE_C_COMPILER=clang`) warning-free at the same counts, `ar -t build/libxud.a` at **39** objects
  (`xud_descriptor.c.o` among them), `nm -u build/xud_test` still listing no `vk*` and no `glfw*` symbol and `ldd` still
  listing no `libvulkan`/`libglfw`/`libGL`/`libEGL` (the rung calls no Vulkan entry point except through a row of Volk's
  device table), `scripts/format.sh --check` clean, and `./build/hello_context` still completing
  (`xud_init_context(640, 480) -> XUD_OK`, `xud_close_context -> XUD_OK`).

  **What landed, and the split that makes the rung testable with no device present**: the rung is two halves and the
  headless run covers the pure one completely — `xud_descriptor_caps_support`/`fill_set_layouts`/`fill_push_range`/`fill_pool_info`/`fill_set_allocate_info`/`fill_buffer_info`/`fill_write_info`/`fill_uniform_block`
  call no driver function at all, and every guard of the driver half fires **before** a loader row is read. The pure test
  drives them against **synthetic** caps (one row per refusal: too few bound sets, no per-stage uniform buffer, no per-set
  uniform buffer, too few per-stage samplers, too few per-set samplers, too few push bytes, a uniform-buffer range too
  small — and one row that fits, to show the gate is a compare rather than sticky) and against synthetic **live** handles
  (a pool, a set-0 layout, a set, a buffer), so the rows are exercised as facts: both layout create-infos stamped and
  `pNext`-cleared with one binding each and both bindings numbered 0 (the contiguous-binding-list rule the create-info
  states for `pBindings`), set 0 one `UNIFORM_BUFFER` of 1 read by both stages, set 1 one `COMBINED_IMAGE_SAMPLER` of
  `XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE` read by the fragment stage only, both `pImmutableSamplers` NULL because the texture
  step owns the samplers, the push range stamped with `stageFlags`/`offset`/`size` and a refusal leaving the row at zero
  rather than half-written, the pool stamped with `maxSets = XUD_DESCRIPTOR_SET_COUNT` and one `VkDescriptorPoolSize` row
  per descriptor type, the set-0 allocate info naming the record's own pool and the record's own set-0 layout (a record
  with a pool and no layout is a refusal — an allocate info that names a pool but no layout names nothing), the buffer
  info stamped with `XUD_DESCRIPTOR_UNIFORM_BLOCK_SIZE`/`XUD_DESCRIPTOR_BUFFER_USAGE`/`VK_SHARING_MODE_EXCLUSIVE` and no
  queue-family row, the write stamped with `dstSet`/`dstBinding`/`descriptorCount`/`descriptorType` and its `pBufferInfo`
  pointing at the record's own `VkDescriptorBufferInfo` row (the create-info-read-after-the-call rule, one rung up), and
  the uniform block's bytes (identity `viewProj`, zeros elsewhere, no lights) — plus the pipeline-layout checks that now
  run against a synthetic descriptor record rather than against nothing.

  **What the GPU test adds against the driver**, and why each half is load-bearing rather than decorative: the caps gate
  answers `XUD_OK` against the **real** caps and the test reads the limits it depends on off them (`maxBoundDescriptorSets`,
  `maxPerStageDescriptorSamplers`, `maxPushConstantsSize`) rather than assuming them; both set layouts, the pool, set 0 and
  the uniform buffer are live, and set 1's set is asserted `VK_NULL_HANDLE`, which is the point of the split; the uniform
  buffer's `XudMemoryRange` is the allocator's first block at offset 0 with its own generation, its `uniformBindOffset`
  equals the range's own offset (the bind is into what the allocator actually handed, not into what was asked for), and
  the write's `pBufferInfo` still points inside the record after the driver call; the allocator's stats after bring-up
  read one block / one live range / the range's own size / a ring cursor still 0, which is the first time anything above
  the allocator has ever held a range; the renderer's pipeline layout is live, i.e. the layout the renderer created
  **names** this rung's handles; and the teardown closes clean and clears. The GPU allocator test's numbers are now a
  **delta on the context's own baseline** (`baseBlocks`/`baseLive`/`baseBytes` read before the test's own requests, the
  first fresh request landing **past** the descriptor's carve, the reuse-after-release check still exactly the coalesced
  free range) — the descriptor's uniform buffer is the first thing the allocator was ever asked for, so the old absolute
  expectations were stale about the tree, not about the allocator.

  **Honest boundary after this step**: the layout exists, set 0 exists, the buffer is bound and the write binds it, and
  the pipeline layout the renderer created names all of it — but **no command binds it**: `xud_frame.c` calls neither
  `vkCmdBindDescriptorSets` nor `vkCmdPushConstants`, the built-in SPIR-V still reads no binding and takes no push
  constant, so the first triangle is still three vertices from `gl_VertexIndex`. What this rung bought is that the batch
  step's `vkCmdBindDescriptorSets` has a set to bind and the shapes step's push constants have a range to push into.
  Set 1's set, every `VkImage` and every sampler are the texture step's, and `xud.h` still declares no create/close
  function that takes an `XudHandle`.

## Steps complete (S12 The GPU-memory allocator rung — one owner of every `VkDeviceMemory`, a free list, a transient ring, one step, landed as one commit, 2026-10-25)

- [x] ~~**GPU memory allocator + suballocation** (**ours**, ring for transient; no `vkCreateBuffer` per frame — the
  VMA question is settled out 2026-10-23, see Decided below) — the **one rung still not in the tree** after S11, and
  the one `void*` sentinel left in `xud_core.h`~~ — **done 2026-10-25**: `src/xud/xud_allocator.h`/`xud_allocator.c`
  land between the swapchain and the renderer, `xud_core.h`'s `allocator` field becomes a typed `XudAllocator*` —
  **the last `void*` sentinel in `XudContext`, so no field of it is a sentinel any more** — `xud_init_context()` opens
  the allocator after the swapchain and `xud_close_context()` closes it after the renderer and before the swapchain
  (a `VkDeviceMemory` is created from a `VkDevice`, so it must die before it, and a submission or a presentation may
  still be reading the blocks), and the CMake source list grows the new `.c`. See AGENTS.md's **Verified allocator
  facts (2026-10-25)** block for the ground (the header facts and the `VkMemoryHeap.flags`/`VkMemoryType.propertyFlags`
  naming trap, the driver's memory rows today vs the S5.4 note's, every validation VUID the layer refuses here and
  the shape the driver accepts silently, the protected-memory rule and the chained-feature reason behind it, the
  block policy and why a request above the bound is a refusal rather than a bigger block, the free list and its
  coalesce, the ring as one recorded block that bump-allocates and never frees, and the teardown guard).

  **Bar met, dated one-liner**: headless **2020 checks / 0 failures** (was 1916), `XUD_GPU_TESTS=1` **2460 / 0** (was
  2284) and validation-clean with `VK_INSTANCE_LAYERS` unset **and** forced to `VK_LAYER_KHRONOS_validation` (the only
  `ERROR/validation` line in either run is still the diagnostics layer's own synthetic headless probe), `-DXUD_SANITIZERS=ON`
  reporting **only** the external allocations the S7/S8/S9/S10/S11 blocks record — headless **72 B / 3 allocations**,
  byte-for-byte the S11 step's numbers, so the allocator's pure half leaks nothing, and GPU **1312 B / 63** (was 1272 B
  / 61: one more libdecor 8 B and one more validation-layer 32 B whose four frames between `malloc` and `xud_device_open`
  are `<unknown module>`, i.e. inside `vkCreateDevice` and not inside lavvk and not inside xudlibc — the GPU allocator
  test opens one more context), `ASAN_OPTIONS=detect_leaks=0` answering 2020 / 2460 with 0 failures and no `runtime error`
  line, the clang build (`-DCMAKE_C_COMPILER=clang`) warning-free at the same counts (it needed one test fix: an all-zero
  `VkMemoryRequirements` handed to a `const VkMemoryRequirements*` parameter is a `-Wuninitialized-const-pointer` error
  there, so the synthetic `ask` is `memset` like every other synthetic in the file), `ar -t build/libxud.a` at **38** objects
  (`xud_allocator.c.o` among them), `nm -u build/xud_test` still listing no `vk*` and no `glfw*` symbol and `ldd` still
  listing no `libvulkan`/`libglfw`/`libGL`/`libEGL` (the allocator calls no Vulkan entry point except through a row of
  Volk's device table), `scripts/format.sh --check` clean, and `./build/hello_context` still completing
  (`xud_init_context(640, 480) -> XUD_OK`, `xud_close_context -> XUD_OK`).

  **What landed, and the split that makes the rung testable with no device present**: the rung is two halves and the
  headless run covers the pure one completely — `xud_allocator_open`/`xud_allocator_select_memory_type`/`xud_allocator_fill_allocate_info`/`xud_allocator_find_range`/`xud_allocator_release`/`xud_allocator_range_get`/`xud_allocator_ring_reset`/`xud_allocator_stats` call no driver function at all, and the driver half's guards (`xud_allocator_acquire`/`xud_allocator_ring_acquire`/`xud_allocator_close`) are read headless too, because every one of them fires **before** a loader row is read. The pure test drives them against **synthetic** caps and synthetic rows (two heaps, seven type rows, one of them protected, one of them naming a heap index that does not exist) so the rules are exercised as facts rather than as driver answers: device-local preference, a host-visible-only request, a heap too small, a protected row skipped, a broken row skipped, `memoryTypeBits == 0` unsupported, the stamp/`pNext`/size/type-index of the create-info, a free-list scan that skips a retired row, a live row, a wrong-type row, a too-big request, the accessor on a live row, the release + coalesce (the survivor is the released row, the merged-away row keeps its tag and bumps its generation twice), a stale release, a stale accessor, a fabricated generation, a cross-block release, the ring rewind, the stats, and the empty-allocator teardown that must not `free()` a host array that was never allocated (`free(NULL)` is undefined, not a no-op — the one bug this step found in its own teardown before the first GPU run).

  **What the GPU test adds against the driver**, and why each half is load-bearing rather than decorative: the heap size
  the caps actually report (23.3 GB, not the S5.4 note's ~4 GB — the machine's Vulkan driver moved, so every number here
  is read off the caps), a device-local type chosen from the caps' seven rows, a first 4 KiB request that creates the
  layer's first block at offset 0, a second 1024-byte request that **suballocates inside that same `VkDeviceMemory`**
  (same block index, same handle, offset at least 4096 and aligned to 256 — the point of the rung, read as a fact), a
  request at `XUD_ALLOCATOR_BLOCK_MAX_SIZE` that opens a **second** block with a different handle and block index 1,
  the stats that say two blocks / three live ranges / exactly `MAX + 4096 + 1024` reserved bytes, a release of two of
  them, a stale release and a stale accessor both refused with `XUD_INVALID_ARG`, a 4 KiB request that **reuses** the
  hole it just freed (same block, same handle, offset 0, generation 1 — the reuse-first rule and the generation bump
  read together), a stats check that the reserved bytes moved by exactly the 1024-byte range that was released and not
  asked for again, three refusals against real caps (above the block bound → `XUD_OUT_OF_RESOURCES`, above the heap →
  `XUD_UNSUPPORTED`, `memoryTypeBits == 0` → `XUD_UNSUPPORTED`, each leaving its out-range untouched), the ring's first
  request creating its one block and recording it in the block array so the teardown frees it with the rest, three
  ring requests moving the cursor 0 → 256 → 1280, a wrap request rewinding to offset 0 in the same block, a request
  larger than the ring refused, `xud_allocator_ring_reset()` rewinding the cursor and the next frame's request reusing
  the first offset, the final stats (three blocks, cursor 256), the validation-clean check on the context's own
  `errorMessages`, and a context close that comes back `XUD_OK` and clears the pointer.

  **Comment sweep, same day**: the step's own bar item 4 ("the file-top comments describe exactly what is in the tree")
  was not met the moment the allocator landed — ten files still said the allocator rung was "not in the tree" or that
  there was "no GPU-memory allocator": `xud_pipeline.h`/`xud_pipeline.c` (the zero-vertex-input reason and the no-depth-attachment
  reason), `xud_renderpass.h`/`xud_renderpass.c` (the same reason), `xud_renderer.h`/`xud_renderer.c` (the "does NOT own"
  list), `xud_handle.h`/`xud_handle.c` (the `record` sentinel's reason) and `xud_core.h`/`xud_core.c` (the swapchain's
  place in the teardown order). Every one now says the allocator IS in the tree (S12) and owns no `VkBuffer` and holds
  no image — nothing above it has asked it for memory — and the reason a slot's `record` is still NULL moved from
  "the allocator and renderer rungs have not landed" to "the batch and object rungs ABOVE them have not", and the
  swapchain's teardown reason from "the last Vulkan rung created below the renderer" to "below the allocator". One
  factual correction came out of the sweep rather than a wording preference: `xud_renderpass.h` had claimed the depth
  TEST and the depth WRITE were on in the pipeline, and `xud_pipeline.c` sets both to `VK_FALSE` — the comment now says
  both are OFF, which is what `xud_pipeline.h` already stated and what makes the first triangle's depth behaviour not
  implementation-dependent. Rebuild after the sweep: headless 2020 / 0 and GPU 2460 / 0, unchanged, format clean.

  **Honest boundary**: `VkDeviceMemory` is allocated, suballocated, grown, released, reused, wrapped and freed against
  real caps with the validation layer clean — but **nothing above the allocator has asked it for anything**. There is
  still no `VkBuffer`, no `VkImage`, no vertex stream, no descriptor set and no batch, so a context that comes back
  `XUD_OK` owns an allocator with **no block in it at all** (`blockCount == 0`, `ringBlockIndex == XUD_ALLOCATOR_NO_BLOCK`,
  both asserted by the GPU context test), no `XudMemoryRange` is held by any rung, no handle names an object, and `xud.h`
  still declares no create/close function that takes one. The descriptor-layout step is what makes the first range a
  rung above the allocator actually holds.

## Steps complete (S11 First triangle end to end — five rungs from SPIR-V bytes to a drawn frame, one step, landed as one commit, 2026-10-25)

- [x] ~~**First triangle end to end on the explicit `VkRenderPass` + `VkFramebuffer` path**: SPIR-V built by
  `glslangValidator`, one graphics pipeline, one command buffer, one render pass + one framebuffer per image,
  acquire/submit/present with semaphores + per-image fence; `vkGetDeviceProcAddr` becomes a loader slot here~~ —
  **done 2026-10-25**: five new module pairs land between the device and the public API — `xud_shader.h`/`xud_shader.c`
  (the shader rung), `xud_pipeline.h`/`xud_pipeline.c` (the pipeline rung), `xud_renderpass.h`/`xud_renderpass.c`
  (the render-pass + framebuffer rung), `xud_frame.h`/`xud_frame.c` (the frame loop) and `xud_renderer.h`/`xud_renderer.c`
  (the **renderer rung**, the owner of all four and the rung `xud_init_context()` wires) — plus `shaders/solid.vert` +
  `shaders/solid.frag` and the CMake step that builds them. See AGENTS.md's **Verified first-triangle facts (2026-10-25)**
  block for the ground (the renderer's open/close order and why the pass must exist before the pipeline, the SPIR-V header
  rule, the `pName`/`renderPass`/`subpass` VUIDs, the extension-gated `*_OP_NONE` stencil values, the attachment-layout
  pattern and its reference VUID, the `vkDestroyRenderPass` IN-parameter, the core-1.2 `vkWaitSemaphores` row that is nil
  at this library's core-1.0 floor, `clearValueCount`, the per-image layout transition and its four barrier facts, the
  no-depth-attachment reason depth test/write are off, and the CMake 4.4.4 quirks the shader step works around).

  **Bar met, dated one-liner**: headless **1916 checks / 0 failures** (was 1739), `XUD_GPU_TESTS=1` **2284 / 0** (was 2068),
  validation-clean with `VK_INSTANCE_LAYERS` unset **and** forced to `VK_LAYER_KHRONOS_validation` (the only `ERROR/validation`
  line in either run is still the diagnostics layer's own synthetic headless probe), `-DXUD_SANITIZERS=ON` reporting **only**
  the external allocations the S7/S8/S9/S10 blocks record (headless 72 B / 3 allocations, byte-for-byte the S10 step's; GPU
  1272 B / 61 — one more libdecor 8 B and one more validation-layer 32 B than the S10 step's 1136 B / 58 because the
  GPU draw test opens one more context, plus one 96 B allocation inside `libwayland-client.so.0` (`wl_display_read_events
  ← wl_display_dispatch_queue_timeout ← wl_display_dispatch_queue ← wl_display_roundtrip_queue`, under `xud_swapchain_close`,
  i.e. the compositor traffic the present step generates), `ASAN_OPTIONS=detect_leaks=0` answering 1916 / 2284 with 0 failures
  and no `runtime error`
  line, the clang build (`-DCMAKE_C_COMPILER=clang`) warning-free at the same counts, `ar -t build/libxud.a` at **37** objects
  (the five new `.c.o` among them), `nm -u build/xud_test`/`nm -u build/hello_context` still listing no `vk*` and no `glfw*`
  symbol and `ldd` still listing no `libvulkan`/`libglfw`/`libGL`/`libEGL`, `scripts/format.sh --check` clean, and `./build/hello_context`
  now completing (`xud_init_context(640, 480) -> XUD_OK`, `xud_close_context -> XUD_OK`) instead of refusing.

  **What landed, and the four things this step had to learn from the validation layer rather from the driver**: the driver
  accepts almost every wrong shape here silently — a pipeline with `renderPass = VK_NULL_HANDLE`, a pass whose stencil ops
  are the extension-gated `*_OP_NONE`, an attachment whose both layouts are `PRESENT_SRC_KHR`, a `clearValueCount` of 0, an
  image left in `UNDEFINED` — so the debug GPU run is the test signal, not the release GPU run (the release run passed the
  whole time while the debug run aborted). The four: (1) **the core-1.4 pipeline binding names the pass**
  (`VUID-VkGraphicsPipelineCreateInfo-dynamicRendering-06576`) — which is why the renderer opens the **pass before the
  pipeline** and destroys it after, and why `XudPipeline` carries a **borrowed** `VkRenderPass`; (2) **`vkWaitSemaphores`
  is core 1.2** and the instance is at the core-1.0 floor, so its device-table row is nil here and the acquire gate moves to
  the submit's `pWaitSemaphores` + `pWaitDstStageMask` pair (the `VkSemaphoreWaitInfo` field and the host-side wait are gone
  from the frame contract); (3) **the swapchain image has no layout until something gives it one**, so `XudFrame` records one
  `VkImageLayout` per image and issues a `vkCmdPipelineBarrier` carrying one `VkImageMemoryBarrier` before the pass — and that
  barrier's members are `oldLayout`/`newLayout` (not `srcLayout`/`dstLayout`, no `flags`), its queue-family indices must both be
  set and equal on a `VK_SHARING_MODE_EXCLUSIVE` image, and its stage masks must support the colour-attachment access bits
  (`ALL_GRAPHICS`; `TOP_OF_PIPE` and `VERTEX|FRAGMENT` are both refused for `COLOR_ATTACHMENT_WRITE`); (4) **`clearValueCount`
  must be at least the highest cleared attachment index + 1** (`VUID-VkRenderPassBeginInfo-clearValueCount-00902`). Two more
  were bugs found before the first GPU run rather than by the layer: the **out-param guard pattern** — an open function must
  refuse a pointer that already owns the object (`*out != NULL` → `XUD_INVALID_ARG`), and the inverted form was in all six new
  entry points, which is what made `xud_init_context()` answer `XUD_INVALID_ARG` for valid extents; and `xud_framebuffer_fill_create_info()`
  had to take the **owning framebuffer** rather than a bare `VkImageView`, because `pAttachments` is read after the call returns
  and must point at storage that outlives it. **Honest boundary**: a triangle is drawn — four frames acquired/recorded/submitted/presented,
  image index moving, per-image fence waited and reset — but it is **one hard-coded `vkCmdDraw` of three vertices generated from
  `gl_VertexIndex`**, with no vertex buffer, no push constant, no descriptor set, no camera, no batch and no depth buffer above or
  beside it; `xud.h` still declares no draw call and no object handle, so nothing a caller can name is drawn yet.

  **Test scaffolding hardened as part of this step** (the item that was still open in `todo.md` before it — "headless
  pure-logic tests green by default, GPU tests opt-in via `XUD_GPU_TESTS=1`, validation-layer output treated as a
  failure signal"): all three halves are in place and all three are exercised by this step at once. The headless run
  stays green with no device present, because the five new rungs' pure halves (the SPIR-V header check, the pipeline /
  renderpass / framebuffer create-info assembly, the submit and present rows) call no driver function. The GPU run is
  opt-in. And the validation-clean rule is now read **in run**, not only as a log grep: the diagnostics layer's log
  callback counts every message it is handed into the struct it was given, and the GPU checks assert that the context's
  own `errorMessages` is still 0, so a validation complaint anywhere in a GPU run fails the run rather than going unn
  oticed (the shell grep is still run alongside, as a second opinion). The 12 GPU checks this step adds over the S10
  step's count are the failure shapes of the frame loop against **this** context's live objects rather than against
  synthetic ones — a NULL out-index, a NULL pipeline, a NULL pass, a NULL framebuffer array, a zero row count, a row
  count that is not the swapchain's, a NULL renderer, and the renderer's own count-vs-array rule — plus the two state
  checks that say every one of those refusals fired **before** the acquire (`framesDrawn` still 0, `lastFrameFence`
  still `VK_NULL_HANDLE`) and the two that say a refused renderer draw added no frame.

## Steps complete (S10 Handle layer — the one `XudHandle` type + per-kind slot arrays, one step, landed as one commit 2026-10-24)

- [x] ~~**S10 Handle layer**: the one `XudHandle` type — `{ uint32_t index; uint32_t generation; uint32_t kind; }` (12 B,
  no padding, `static_assert(sizeof == 12u, "…")`) + per-kind slot arrays (textures, fonts, meshes, materials, models) +
  the `XUD_HANDLE_KIND_*` tag (`XUD_HANDLE_KIND_NONE = 0u` never stored in a live slot) + validation in order index-bounds
  → generation → kind, failure `XUD_INVALID_ARG`; slot allocation, generation bump on reuse, stale-handle rejection and
  cross-kind rejection; the pure half is headless-testable by construction, the Vulkan handles stay inside the slot
  records~~ — **done 2026-10-24**: `src/xud/xud_handle.h`/`src/xud/xud_handle.c` land as the rung **above the swapchain
  and below the renderer**, and `xud_core.c` now opens one table as the last rung of the bring-up ladder and closes it
  as the first rung of the teardown order. See AGENTS.md's **Verified handle layer facts (2026-10-24)** block for the
  ground (the three-part validation order and why the live bit is checked last, the capacity bound and why it is this
  layer's own ceiling, the reuse-first/generation-bump rule, and the fact that no slot record exists yet).

  **What landed**: `xud.h` grows the public currency — `XudHandle` is three `uint32_t` (12 B, no padding, asserted
  beside the type), `XUD_HANDLE_KIND_NONE`/`TEXTURE`/`FONT`/`MESH`/`MATERIAL`/`MODEL`/`MAX`/`COUNT` are the tag values
  and the array bound, and the ownership comment says the handle is the one thing a caller holds while the table behind
  it is internal. `xud_handle.h` owns the storage: `XudHandleSlot` is `{ occupied, generation, kind, record }` — the
  live bit, the value a handle must carry to name the slot at all, the tag that makes the check exact, and the sentinel
  of the rung that is not in the tree yet (`record` is NULL on every live slot for as long as this layer is the top of
  the tree, so `xud_handle_table_close()` frees a table without walking it because there is nothing in it to close); and
  `XudHandleTable` is one array per kind, indexed by the kind value itself, so row 0 (`NONE`) exists and stays NULL
  forever, and an array is NULL until the first allocation of that kind — the "no object of that kind was ever created
  in this context" state, which is what keeps a context that draws only meshes from paying for a font array. The
  contract is seven pure functions: `xud_handle_table_open()` / `xud_handle_validate()` / `xud_handle_equal()` /
  `xud_handle_alloc()` / `xud_handle_free()` / `xud_handle_table_live_count()` / `xud_handle_table_close()`, plus one
  static growth helper (`xud_handle_reserve()`). **The validation order is the load-bearing half**: expected-kind range
  → array exists → index below the high mark → generation equality → tag equality → live bit, and the expected kind is
  the **entry point's** kind rather than the handle's, which is what makes a cross-kind handle a caught error instead
  of a read of the wrong array; the live bit is checked last because generation and tag can both match a **free** slot
  for a handle a caller fabricated, and a decoded handle must always name a live slot. `xud_handle_alloc()` is reuse-
  first (first free slot below the high mark, generation bumped so every handle that named it before the release that
  freed it stops naming it) or the high mark itself, growing that kind's array by the doubling sequence the rest of
  this tree uses (`XUD_HANDLE_SLOT_MIN_CAPACITY` 16 → `XUD_HANDLE_SLOT_MAX_CAPACITY` 4096, with the two asserts that
  make the sequence land on the bound exactly rather than stepping past it and refusing a slot one row too early); at
  the bound a wanted slot is `XUD_OUT_OF_RESOURCES`, not a quiet cut, and on every error path the out-handle is left
  untouched. `xud_handle_free()` validates against the handle's **own** tag, so a stale handle, a cross-kind handle
  and a fabricated one are all refused there rather than double-releasing a slot or bumping a generation twice. `xud_core.h`
  grows a typed `XudHandleTable*` field and its teardown comment grows a handle-table step **before** the swapchain
  (the handle table is the top of the tree: a live table can name objects the rungs below are about to retire), and
  `xud_core.c` opens the table after the swapchain and closes it first — `allocator` stays the one sentinel field left.
  `CMakeLists.txt` adds `xud_handle.c` to the archive (**32** objects).

  **Bar met** (2026-10-24, gcc 16.2.1 + clang 22.1.8): `gcc -fsyntax-only -std=c11 -Wall -Wextra -Wpedantic -Werror` clean
  over the whole tree, `./scripts/format.sh --check` clean, headless **1739/0** (was 1642) and GPU **2068/0** (was 1969)
  validation-clean with `VK_INSTANCE_LAYERS` unset **and** forced to `VK_LAYER_KHRONOS_validation` (the only `ERROR/validation`
  line in either run is still the diagnostics layer's own synthetic headless probe), sanitizers reporting **only** the
  external allocations AGENTS.md records — headless **72 B / 3 allocations** and GPU **1136 B / 58**, byte-for-byte the
  S9 step's numbers, so the handle layer leaks nothing (and note that with leak detection on the sanitizer binary's
  buffered stdout is discarded by LSan's exit path, so the check counts were read with `ASAN_OPTIONS=detect_leaks=0`, which
  answers 1739/0 headless and 2068/0 GPU); the clang build (`-DCMAKE_C_COMPILER=clang`) is warning-free and answers the
  same counts, `ar -t build/libxud.a` lists **32** objects with `xud_handle.c.o` among them, and `nm -u build/xud_test`
  still lists no `vk*`/`glfw*` symbol while `ldd` lists no `libvulkan`/`libglfw`/`libGL`/`libEGL` — the handle layer
  includes no Vulkan header and calls no driver function at all, which is what makes the whole rung headless-testable
  by construction. The GPU context test grows two checks of its own: the context owns a table, and the table's per-kind
  arrays are all NULL in a context that created no object — the empty-table state, asserted rather than assumed.

  **Honest boundary**: the handle currency and the per-kind slot arrays are in the archive and tested, but a slot's
  `record` field is the NULL sentinel of a rung that does not exist — there is still no allocator, no render pass, no
  framebuffer and no batch, so **no handle names an object yet**. `xud.h` still declares no create/close function that
  takes a handle (the public API stays narrow on purpose: the handle type and its tag values are public because a caller
  holds them, the table and these seven functions are internal like every layer below the public API). That is the
  *Next* item above: the first triangle end to end on the explicit `VkRenderPass` + `VkFramebuffer` path.

## Steps complete (S9 Swapchain rung + context wiring — one step, landed as one commit 2026-10-24)

- [x] ~~**S9 Swapchain**: create the `VkSwapchainKHR` for the surface, recreate it on window resize, never leak the old
  one; explicit-pass path — the `VkFramebuffer` array is per swapchain image and is recreated with the swapchain~~ —
  **done 2026-10-24**: `src/xud/xud_swapchain.h`/`src/xud/xud_swapchain.c` land as the rung **above the device and
  below the renderer**, and `xud_core.c` now creates one as the last rung of the bring-up ladder and destroys one as
  the first rung of the teardown order. See AGENTS.md's **Verified swapchain facts (2026-10-24)** block for the ground
  (the three surface queries probed through the validation layer, the create-info fields the validation layer catches,
  the `oldSwapchain` + explicit-destroy rule, the `maxImageCount == 0` answer, the `4294967295` "undefined" `currentExtent`,
  the format/present-mode lists this surface advertises, and the two Vulkan names that do **not** exist here).

  **What landed**: `XudSurfaceCaps` records the three surface queries' answers — `VkSurfaceCapabilitiesKHR` plus fixed
  arrays for the formats (`XUD_SWAPCHAIN_MAX_SURFACE_FORMATS` 32) and the present modes (`XUD_SWAPCHAIN_MAX_PRESENT_MODES`
  8) — and `xud_swapchain_query_surface()` refuses `VK_INCOMPLETE` and an over-count at those bounds with `XUD_OUT_OF_RESOURCES`
  rather than truncating, because unlike the extension lists these two bounds are the storage of the struct being filled,
  not a heap array that can grow. The three selection rules are **pure** (`xud_swapchain_choose_image_count()` / `xud_swapchain_select_format()`
  / `xud_swapchain_select_present_mode()`) and `xud_swapchain_fill_create_info()` calls them, so the create-info and the
  record of it cannot disagree — there is exactly one derivation path from the caps to the create-info. `xud_swapchain_open()`
  is the whole ladder for one surface or nothing (query → assemble → create → query the images, and a swapchain nobody can
  get an image from is destroyed before the struct goes away). `xud_swapchain_recreate()` is the one place a swapchain is
  replaced: create carrying `oldSwapchain` → **destroy the old explicitly** (the spec retires its images as part of the
  create but the handle still has to be destroyed, and a driver that only retired it implicitly would leak it) → query the
  new images → swap the record; a failed create leaves the old swapchain alive and unchanged (probed), and a successful
  create followed by a failed image query destroys the new one and leaves the struct in the no-swapchain state with its
  record still at the extent it **had** a swapchain for, so the retry above cannot answer `XUD_ALREADY` with nothing in
  hand. `xud_swapchain_close()` is guard → destroy → free: it calls `xud_device_wait_idle()` first, because a presentation
  may still be in flight against these very images, and a lost device (`XUD_LOST`) is still destroyed and still reported.
  `xud_swapchain_images_get()` is the accessor that makes a caller above hold an **index** rather than a `VkImage` — a
  recreate replaces the whole array, so a handle kept across one is refused, not dangling. `XudSwapchain`'s one heap member
  is the image array, **freed and never destroyed**: there is no `vkDestroyImage` call anywhere in this layer, because the
  images belong to the swapchain and `vkDestroySwapchainKHR` retires them. `xud_core.h`'s `swapchain` sentinel becomes a
  typed `XudSwapchain*` (`allocator` is now the only sentinel field left), `xud_core.c` calls `xud_swapchain_open()` after
  the surface and `xud_swapchain_close()` before `xud_device_close()`, and `xud.h`'s two doc comments say so. `CMakeLists.txt`
  adds `xud_swapchain.c` to the archive (31 objects).

  **Bar met** (2026-10-24, gcc 16.2.1 + clang 22.1.8): `gcc -fsyntax-only -std=c11 -Wall -Wextra -Wpedantic -Werror` clean
  over the whole tree, and the same under clang with no warning over the vendored set; headless **1642/0** (was 1529),
  `XUD_GPU_TESTS=1` **1969/0** (was 1786) and validation-clean with `VK_INSTANCE_LAYERS` unset **and** forced to
  `VK_LAYER_KHRONOS_validation` (the only `ERROR/validation` line in either run is still the diagnostics layer's own
  synthetic headless probe); `-DXUD_SANITIZERS=ON` reporting **only** the external allocations AGENTS.md records (see
  *Verified swapchain facts* — the libdecor/libgdk 8 B and the validation layer's 32 B per `vkCreateDevice`, neither of
  it ours, no suppression installed); `ar -t build/libxud.a` lists **31** objects with `xud_swapchain.c.o` among them;
  `nm -u build/xud_test` lists no `vk*` and no `glfw*` symbol and `ldd` lists no `libvulkan`, no `libglfw`, no `libGL`/`libEGL`;
  `./scripts/format.sh --check` clean.

  **Honest boundary**: the swapchain is created, recreated and destroyed, and the context owns it — but **nothing is drawn
  into its images yet**: no command buffer, no render pass, no framebuffer, no acquire/present pair, and the `VkFramebuffer`
  array the item speaks of does not exist yet, so "recreated with the swapchain" is a rule the *Next* step writes against,
  not a rule this step could test. `xud_core.h`'s `allocator` field is the one rung still not in the tree.

## Steps complete (S8 Context ownership + teardown — one step, landed as one commit 2026-10-24)

- [x] ~~**S8 Ownership + teardown**: the context owns loader/instance/device/**window** (the surface lives inside the windo
  w now, not in a context field of its own); destroy order device → surface → window → instance; `goto fail` ladder leaks n
  othing on partial init; a GPU test that creates then destroys and stays validation-clean~~ — **done 2026-10-24**: `src/xu
  d/xud_core.h`/`xud_core.c` stop being a shell and become **the owner** — `xud_init_context()` runs the whole bring-up ladde
  r to the surface and `xud_close_context()` is the one teardown order that hands it all back. See AGENTS.md's **Verified con
  text ownership facts (2026-10-24)** block for the full ground (the bring-up order is the order the dependencies force, not t
  he bottom-to-top layer order, and it is written in the header comment where it can be read: loader → diagnostics → **windo
  w** → GLFW's required instance extensions → instance carrying them → messenger → physical through THAT instance → GLFW's p
  resentation check → device → surface; the window comes **before** the instance because `glfwCreateWindowSurface` resolves v
  kCreateWaylandSurfaceKHR through `vkGetInstanceProcAddr` **against the instance it is handed**, which is the S7 fact one ru
  ng up and the whole reason `xud_instance_create()` has an `extraNames`/`extraCount` pair; a merged instance enables four na
  mes here, not five, because GLFW's Wayland list is exactly `VK_KHR_surface` + `VK_KHR_wayland_surface` and `VK_KHR_surface` i
  s already wanted — the GPU test asserts the dedup directly; teardown is `device → surface → window → physical → instance → d
  iagnostics → loader`, the swapchain slots before the device when it lands, and every step is load-bearing: a `VkDevice` is cr
  eated from a `VkPhysicalDevice` so it goes first, `vkDestroySurfaceKHR` needs the owning `VkInstance` alive and `xud_window_c
  lose()` **refuses** a window whose surface is live so the surface goes before the window, a `VkPhysicalDevice` is a query resu
  lt **of** an instance so it goes before it, and `xud_instance_destroy()` tears the messenger down through the still-alive insta
  nce before `xud_diagnostics_close()` frees the struct; `xud_init_context()`'s failure path calls `xud_close_context()`, so a p
  artial bring-up leaks nothing and there is exactly one teardown order in the tree; `xud_close_context()` is idempotent per run
  g and returns the **first** non-`XUD_OK`, non-`XUD_ALREADY` result, so a lost device (`XUD_LOST`) and a NULL loader row are rep
  orted, not hidden, and the remaining rungs still run; the presentation check is a **refusal**, not a bad argument, and it is as
  ked about the physical layer's **chosen** family, the present half of the one-family rule; `XUD_VALIDATE` is snapshotted into th
  e context and the diagnostics struct is opened on that copy; the context carries **no queue field at all** — `XudQueue` is a reco
  rd, there is no `vkDestroyQueue`, and both records live inside `XudDevice`).

  **What landed**: `xud_core.h`'s `void *device` sentinel becomes a typed `XudDevice*`, its `void *queueGraphics`/`void *queuePre
  sent` sentinels are **deleted** rather than filled (a context field for them would be a second, unowned copy of state the device
  already owns, and the public API has no way to name a queue), `XudWindow* window` moves above `XudInstance*` to match the creation
  order, and the ownership + teardown comments say exactly what the code does. `xud_core.c` grows the ladder: `xud_window_open()` → `x
  ud_window_required_instance_extensions()` → `xud_instance_create()` with those names → `xud_diagnostics_create_messenger()` → `xud_phys
  ical_open()` → `xud_window_presentation_support()` (a `false` answer is `XUD_UNSUPPORTED`, part-way through, unwound through the one teardown
  order) → `xud_device_open()` → `xud_window_surface_create()`, with the `goto fail` ladder landing in `xud_close_context()` rather than in a per-ru
  ng unwind. `xud.h`'s two doc comments now describe the completed bring-up and the reverse teardown instead of promising `XUD_UNSUPPORTED`. `hello_conte
  xt.c`'s comment says the bring-up is complete to the surface and what is still open below it. `xud_test.c` gains `xud_test_gpu_context()` — the one place in
  the run that creates a **context**, asserting what a context owns field by field (loader open, diagnostics present when validation was asked for and its e
  rror counter still zero, window live at the context's extent, instance carrying GLFW's wayland name with `VK_KHR_surface` enabled **exactly once**, physical wi
  th a chosen family, device whose queue row **is** that family and whose priorities pointer is live, both role queues resolved, the loader bound to this instance a
  nd this device, the surface inside the window naming the context's own instance, and the swapchain/allocator sentinels NULL because those rungs are not in the tree)
  — plus 25 full create+destroy cycles. The old `init/*` expectations that asserted `XUD_UNSUPPORTED` for valid extents are gone: valid extents now assert **eithe
  r** outcome, a completion must close back to nothing and a refusal must hand back nothing. `xud_test_no_leak()` no longer runs 1000 full cycles — it runs 1000 **rej
  ected** extents, which is the leak-free argument it was ever making — because a completed context is what `xud_init_context()` answers on this machine now, and the 1
  000 full cycles moved to the GPU gate as 25 cycles that are actually a real window and a real `VkDevice` each.

  **Bar met (2026-10-24)**: gcc **and** clang builds green from scratch (`-DCMAKE_C_COMPILER=clang`, warning-free, same counts), headless **1529 checks / 0 failures** (w
  as 525) and GPU **1786 / 0** (was 719), validation-clean with `VK_INSTANCE_LAYERS` unset **and** forced to `VK_LAYER_KHRONOS_validation` (the only `ERROR/validation` line i
  n either run is still the diagnostics layer's own synthetic headless probe), `ar -t build/libxud.a` still lists **30** objects (this step added no module — `xud_core.c` was alr
  eady in the archive), `nm -u build/xud_test` and `nm -u build/hello_context` still list no `vk*` and no `glfw*` symbol and `ldd` lists no `libvulkan`, no `libglfw` and no `libGL`/`l
  ibEGL` for either. The sanitizer runs report **only** allocations whose whole stack is outside this tree: the 8-byte libdecor/libgdk leak the S7 block records (GLFW `dlopen`s libdecor when it is present) — which now appears in the **headless** run too, because a completed context creates a real window there — and, in the debug config only, **32 bytes per `vkCreateDevice`** allocated inside the **validation layer** (stack: `malloc ← <unknown module> ×4 ← xud_device_open at the `vkCreateDevice` call line`; a `-DXUD_DEBUG=OFF` config, which asks for no layer at all, reports only the libdecor leak, headless and GPU alike). Nothing in xudlibc allocates, frees or can see either one, so there is nothing to fix and **no suppression is installed** to make the runs look clean.

## Steps complete (S7 The GLFW window + surface rung — one step, landed as one commit 2026-10-23)

- [x] ~~**S7 GLFW window + surface**: `glfwCreateWindowSurface`, `glfwGetRequiredInstanceExtensions` merged into the
  instance extension list, `glfwGetPhysicalDevicePresentationSupport` checked against the chosen queue family, `vkDestroySurfaceKHR`
  on teardown~~ — **done 2026-10-23**: `src/xud/xud_window.h` + `src/xud/xud_window.c` land as the **window rung**, above
  the instance and below the swapchain, and `xud_instance.h`/`xud_instance.c` grow an `extraNames`/`extraCount` parameter
  so the instance can carry the names GLFW needs. See AGENTS.md's **Verified window + surface facts (2026-10-23)** block
  for the full ground (the GLFW Vulkan declarations sit inside `#if defined(VK_VERSION_1_0)`, so `<vulkan/vulkan.h>` must
  precede `<GLFW/glfw3.h>`; GLFW's required list on the Wayland backend is exactly `VK_KHR_surface` + `VK_KHR_wayland_surface`,
  so a merged instance enables 4 names, not 5; `glfwCreateWindowSurface` resolves `vkCreateWaylandSurfaceKHR` through
  `vkGetInstanceProcAddr` **against the instance it is handed**, so an instance without GLFW's window-system extension
  answers `VK_ERROR_EXTENSION_NOT_PRESENT` and the presentation query answers `GLFW_FALSE`; a `VkPhysicalDevice` belongs
  to the instance it was queried through and the validation layer aborts when it is reused across an instance recreation;
  `glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API)` is required or `glfw/src/vulkan.c` 110–113 refuses the window with
  `VK_ERROR_NATIVE_WINDOW_IN_USE_KHR`; `glfwCreateWindow` takes `int`, so `XUD_WINDOW_MAX_EXTENT` is `(uint32_t)INT_MAX`;
  there is no `glfwDestroyVulkanSurface` in 3.5, so teardown is the loader's `vkDestroySurfaceKHR` row, checked **before**
  the create; presentation support is asked about the physical layer's **chosen** queue family, the present half of the
  one-family rule; GLFW's state is process-level, so `xudGlfwLiveWindowCount` — the one piece of state in this tree that
  is not inside `XudContext` — decides when `glfwTerminate()` may run).

  **What landed**: `XudWindow` owns both halves that must die together — the `GLFWwindow*` and the `VkSurfaceKHR` — plus
  the extent asked for, the instance the surface was created for (borrowed, for the teardown call that needs it) and
  GLFW's required-name list copied into fixed buffers it owns (`glfwGetRequiredInstanceExtensions` returns pointers into
  storage GLFW keeps only until `glfwTerminate()`). `xud_window_open`/`xud_window_close` are GLFW's create/destroy,
  `xud_window_surface_create`/`xud_window_surface_close` are GLFW's create and the loader's destroy row, `xud_window_presentation_support`
  is GLFW's window-system query, and `xud_window_required_instance_extensions`/`xud_window_check_instance_support` are pure
  (no GLFW call, no driver call — the headless run covers them completely). `xud_window_close()` **refuses** a window whose
  surface is still live (`XUD_INVALID_ARG`), which is what makes the teardown order surface → window → instance a property
  of the layer rather than of the caller, and `xud_core.h`'s surface sentinel becomes a typed `XudWindow*` for the same
  reason: a `VkSurfaceKHR` cannot be held without the window it was created for. `xud_core.c` passes `NULL, 0u` for the
  window layer's required names — `xud_init_context()` creates no window, so there is no window-system extension to ask
  the instance for, and wiring the context's window field is S8's.

  **Bar met (2026-10-23)**: gcc **and** clang builds green from scratch (`-DCMAKE_C_COMPILER=clang`, warning-free over
  the vendored set and over the new TU), headless **525 checks / 0 failures** (was 472) and GPU **719 / 0** (was 623),
  validation-clean with `VK_INSTANCE_LAYERS` unset **and** forced to `VK_LAYER_KHRONOS_validation` (the only `ERROR/validation`
  line in either run is still the diagnostics layer's own synthetic headless probe), `-DXUD_SANITIZERS=ON` green headless
  (525 / 0, no report) and the GPU run reporting **only** the one external 8-byte leak inside libdecor's GTK plugin that
  this layer does not own (recorded in `xud_window.c`'s file-top comment and in AGENTS.md, no suppression installed),
  `ar -t build/libxud.a` lists **30** objects with `xud_window.c.o` among them, `nm -u build/xud_test` lists no `vk*` and
  no `glfw*` symbol, `ldd` lists no `libvulkan`/`libglfw`/`libGL`/`libEGL`, and `./scripts/format.sh --check` is clean.
  **Honest boundary**: the window and its surface exist in the GPU test run, not in the context — `xud_init_context()` still
  creates neither and `xud_core.h`'s `window` field stays NULL; there is still no swapchain, no allocator, and nothing below
  the physical layer is wired into `XudContext`. That wiring is S8's ownership step.

## Steps complete (S6.6 The vendored GLFW tree built into the archive — Wayland backend, one step, landed as one commit 2026-10-23)

- [x] ~~Build the **vendored GLFW** tree (`src/external/glfw`, 3.5.1) into the library on the **Wayland** backend~~ —
  **done 2026-10-23**: `CMakeLists.txt` gains a **Vendored GLFW 3.5.1 (Wayland backend)** section between the flags and
  the `xud` target, and the window/input rung is now compiled **in** rather than linked from the Arch package. **The
  source set is upstream's `glfw/src/CMakeLists.txt` read for `GLFW_BUILD_WAYLAND=ON`, `GLFW_BUILD_X11`/COCOA/WIN32=OFF
  on Linux** — we use none of upstream's CMake, we transcribe what it would compile: the always-compiled base set
  (`context.c init.c input.c monitor.c platform.c vulkan.c window.c` + the four `null_*` halves the `GLFW_PLATFORM` init
  hint can select at runtime), the Linux/POSIX time/thread/module bits (upstream 12–16, the non-APPLE non-WIN32 branch)
  and the `posix_poll.c` half upstream shares between X11 and Wayland, the Wayland half (`wl_init.c wl_monitor.c wl_window.c`
  + `wl_platform.h`, upstream 44–48) and `linux_joystick.c` (upstream 50, gated on `(X11 or Wayland) and __linux__`).
  `win32_*`/`cocoa_*`/`macos_*`/`nsgl_*`/`glx_context.c`/`x11_*`/`xkb_unicode.c` are not in the build — **`GLFW_BUILD_X11`
  stays OFF, so there is still no X11 path of our own and no RGFW**.

  **`egl_context.c` and `osmesa_context.c` are in, not out** — the trap that decided this: `wl_window.c` and the `null_*`
  halves reference `_glfwInitEGL`/`_glfwCreateContextEGL`/`_glfwInitOSMesa`/`_glfwTerminateEGL` **by name**, so leaving
  them out is a link error, not a smaller binary (probed: dropping them fails the link, keeping them does not pull in a
  GL or EGL library — GLFW `dlopen`s those at runtime, `ldd` lists no `libGL`/`libEGL`). `glfwCreateWindowSurface`
  refuses a window whose client API is not `GLFW_NO_API`, so the Vulkan path never runs through them either: they are
  there because the linker needs the symbols, not because the library wants GL.

  **The Wayland protocol pairs are generated at build time**, from the 9 XMLs GLFW ships in `deps/wayland/`, by
  `wayland-scanner` (`find_program` + a `FATAL_ERROR` when missing — upstream's own rule, upstream 75–77) into
  `build/glfw-wayland/`, so **nothing generated lands next to the vendored sources**, which are never edited in place
  (see *Settled decisions*: a version bump is a re-vendor commit). **Naming trap found while landing this**: the
  `-client-protocol-code.h` files are **not compiled as their own TUs** — CMake classifies a `.h` source as a header and
  does not compile it, which is also why upstream's `target_sources(glfw PRIVATE ${header_file} ${code_file})` exists
  only to force generation — and `wl_init.c` 59–91 `#include`s each `-code.h` inside its own body, so the protocol stubs
  (`wp_viewporter_destroy`, `xdg_wm_base_add_listener`, `zwp_pointer_constraints_v1_destroy`, …) are local `t` symbols
  **inside `wl_init.c.o`**, which is what `nm` shows there.

  **The vendored TUs are not compiled in the library's warning mode**, and that is a fact about GLFW, not a shortcut:
  GLFW 3.5.1 is a hard error under `-Wpedantic` (the `dlsym` object-pointer ↔ function-pointer casts in `posix_module.c`
  50, the same ISO C rule AGENTS.md records for our own replaced loader) and under `-Wextra` (the unused parameters every
  `null_*` half and every Wayland protocol callback carries). Upstream asks for `-Wall` only, as C99 (`glfw/src/CMakeLists.txt`
  130); here the tree is compiled as **C11** — the project's standard, verified clean with gcc 16.2.1 **and** clang 22.1.8 —
  with `-Wall -Werror` kept and `-Wextra`/`-Wpedantic` switched off per target (`-Wno-extra -Wno-pedantic`), so the vendored
  bucket still has to be warning-clean under `-Wall`. `-D_DEFAULT_SOURCE` is upstream's own Linux define (`glfw/src/CMakeLists.txt`
  141): `-std=c11` sets `__STRICT_ANSI__`, which is what would otherwise hide the glibc POSIX extensions `wl_init.c`/`wl_window.c`
  need; `-D_GLFW_WAYLAND` is upstream's backend define (54–55) — the `GLFW_BUILD_WAYLAND` **option** itself exists only in
  upstream's build, so our target sets the define it produces; `HAVE_MEMFD_CREATE` comes from CMake's own
  `check_function_exists(memfd_create)` (upstream 66–70) and is present here, so `wl_window.c` 105–106's `memfd_create`
  shared-memory hint is compiled in.

  **`glfw_vendor` is an OBJECT library, so its objects go into `libxud.a` rather than into a second archive** — `ar -t
  build/libxud.a` lists **29** objects (`volk.c.o` + 21 GLFW objects + the 7 xud module objects) and there is still
  exactly one library to link. **The Wayland link flags are a property of `xud`, not of `glfw_vendor`**, and that is the
  second trap: an object library's own link dependencies are not propagated to the targets that use it, and a static
  archive records no dependencies of its own — verified both ways here, `target_link_libraries(glfw_vendor PRIVATE …)`
  and `PUBLIC …` both left every executable's link line at `libxud.a` only with `ldd` at libc only, so a GLFW call would
  have failed to link. The four libs are therefore named on `target_link_libraries(xud PUBLIC glfw_vendor …)`, from
  pkg-config's `GLFW_WAYLAND_LDFLAGS` and **not** its `GLFW_WAYLAND_LIBS` (verified: that form expanded to nothing — it is
  only filled for the `_DEBUG`/`_RELEASE` variants — and `_STATIC_LDFLAGS` would add `-lrt`, not a library on this machine).
  `ldd` now lists `libwayland-client.so.0`, `libwayland-cursor.so.0`, `libwayland-egl.so.1`, `libxkbcommon.so.0` and
  `libffi.so.8` (wayland-client's own dependency) for both executables.

  **The Vulkan-free link rule is unchanged and still holds**: `nm -u build/xud_test` and `nm -u build/hello_context` list
  no `vk*` and no `glfw*` symbol, and `ldd` lists no `libvulkan`, no `libglfw` and no `libGL`/`libEGL` — there is still no
  `-lglfw` and no `-ldl` anywhere (`.gitignore` gained `build-*/` so the clang second-opinion build directory stays out of
  git alongside `build-san/`).

  **Bar met**: gcc **and** clang (`-DCMAKE_C_COMPILER=clang`, `build-clang/`) build everything from scratch with no warning
  line; `gcc -fsyntax-only -std=c11 -Wall -Wextra -Wpedantic -Werror -I src/xud -I src/external` clean over `src/xud`; headless
  **472 checks / 0 failures** and `XUD_GPU_TESTS=1` **623 / 0** — **unchanged from S6.5, because no test calls GLFW yet** —
  validation-clean with `VK_INSTANCE_LAYERS` unset and forced to `VK_LAYER_KHRONOS_validation` (the only `ERROR/validation`
  line is still the diagnostics layer's own synthetic headless probe), `-DXUD_SANITIZERS=ON` green both ways (472 / 623, no
  report), `./scripts/format.sh --check` clean (it covers `src/xud tests examples` only, never `src/external/`), and AGENTS.md
  gains the *Verified GLFW Wayland build* block, a new *Second opinion (clang)* command row, the corrected *Present today*
  / *Repo layout* / *Toolchain* rows and the two GLFW *Settled decisions* bullets.

  **Honest boundary**: the vendored tree **builds and links**, and nothing uses it yet — nothing in `src/xud/` includes
  `GLFW/glfw3.h`, no test calls a GLFW function, and there is still no window, no `VkSurfaceKHR` and no presentation check.
  That is the S7 `Now` item (`glfwCreateWindowSurface` / `glfwGetRequiredInstanceExtensions` / `glfwGetPhysicalDevicePresentationSupport`
  / `vkDestroySurfaceKHR`), and the reason the test counts did not move.

## Steps complete (S6.5 Loader rework onto vendored Volk — one step, landed as one commit 2026-10-23)

- [x] ~~Loader rework onto **vendored Volk** (`src/external/volk.h` + `volk.c`)~~ — **done 2026-10-23**: the hand-maintained 20-slot `XudLoader` and its `xud_loader_open`/`resolve`/`bind`/`is_open` are replaced by a **thin per-context wrapper over Volk's tables** — the wrapper is kept, the per-name machinery is dropped. **The decision and its reason**: Volk's own entry points are process-wide `extern PFN_vkXxx vkXxx;` globals, so a layer that called them directly could not run two contexts side by side (one context's `volkFinalize()` would clear rows another context is still calling through), and Volk's `volkLoadInstance`/`volkLoadDevice` write exactly those globals. `XudLoader` therefore stays the context's one mutable table, but its contents are now **Volk's two tables plus three loader-level rows**, not 20 hand-picked slots: `struct VolkInstanceTable instanceTable` (127 entries as compiled here, filled by `xud_loader_bind_instance`) + `struct VolkDeviceTable deviceTable` (710 entries, filled by `xud_loader_bind_device`) + `createInstance`/`enumerateInstanceExtensionProperties`/`enumerateInstanceLayerProperties` copied out of Volk's globals at open time (that copy is what makes the wrapper per-context: the globals are Volk's, the rows the loader layer calls are ours). The loader-level row count is still `XUD_LOADER_GLOBAL_SLOT_COUNT` 3 and `xud_loader_open()` still refuses a loader that did not answer all three. **Contract now**: `xud_loader_open` / `xud_loader_close` / `xud_loader_is_open` / `xud_loader_instance_bound(loader, instance)` / `xud_loader_device_bound(loader, device)` / `xud_loader_bind_instance(loader, instance)` / `xud_loader_bind_device(loader, device)`; **gone**: `XUD_LOADER_SLOT_COUNT`, `XUD_LOADER_NAMES`, `XudPfnBits`, `xud_loader_resolve()`, the per-name `xud_loader_bind()`, `resolvedCount`, `getInstanceProcAddr`. **Bound state is handle equality, not a boolean**: the loader records `VkInstance boundInstance` + `VkDevice boundDevice`, and a bound-check is `loader->boundInstance == instance` (plus non-NULL) — so a stale handle across an instance recreation fails the compare before any row is read, which is the same guarantee the old resolve-on-demand rule gave the messenger pair, now for every instance-level row at once. `xud_loader_bind_instance()` is a **rung**: it clears the device table and nulls `boundDevice` when it re-binds, because a device table filled for a dead instance is a table of pointers into a dead layer. `xud_loader_bind_device()` requires `boundInstance != VK_NULL_HANDLE` and a non-NULL `instanceTable.vkGetDeviceProcAddr` row, and sets Volk's global `vkGetDeviceProcAddr` from **this loader's** instance table before `volkLoadDeviceTable()` — that call resolves through the global, so without the set a second context would fill the first context's device table with the second instance's rows. `xud_loader_close()` calls `volkFinalize()` and `memset`s both tables before freeing, so a closed loader has no live row anywhere. **The guards the headless run covers**: a bind with no handle is `XUD_INVALID_ARG`, a device bind with no instance bound — or with a NULL `instanceTable.vkGetDeviceProcAddr` row — is `XUD_UNSUPPORTED` (the rung cannot resolve, that is not a bad argument), and a refused bind leaves every table row NULL.

  **The `VK_NO_PROTOTYPES` include order landed here**: `volk.h` errors `To use volk, you need to define VK_NO_PROTOTYPES before including vulkan.h` when `VULKAN_H_` is already in (volk.h 17–18) and defines the macro itself when it is not (25–26), so **`#include "volk.h"` comes immediately before `<vulkan/vulkan.h>`** in every Vulkan-bearing TU — added to `xud_loader.h`, `xud_vulkan.h`, `xud_diagnostics.h`, `xud_instance.h`, `xud_physical.h`, `xud_device.h` (and `xud_loader.h` needed `<assert.h>` for its `static_assert`s, which is what the first build's `expected declaration specifiers` errors were about). `src/external/volk.c` compiles clean with gcc **and** clang under `-std=c11 -Wall -Wextra -Wpedantic -Werror` as shipped. **The no-Vulkan-symbol rule became a link-time fact**: under `VK_NO_PROTOTYPES` every entry point is a data pointer, so leaving `src/external/volk.c` out of a link leaves those pointers undefined — `ar -t build/libxud.a` now lists **8** objects with `volk.c.o` among them, and `nm -u build/xud_test` / `nm -u build/hello_context` still list no `vk*` symbol while `ldd` still lists no `libvulkan` (Volk's POSIX bring-up is `dlopen("libvulkan.so.1", RTLD_NOW|RTLD_LOCAL)`, volk.c 114, fallback `libvulkan.so` 116 — the same loader the hand-rolled table used, `RTLD_LOCAL` not `RTLD_GLOBAL`).

  **The layers moved with it** (44 `loader->` call sites re-pointed): `xud_instance.c` calls `loader->createInstance` (loader-level row) then `xud_loader_bind_instance()` immediately after the instance exists, and destroys through `loader->instanceTable.vkDestroyInstance` guarded by `xud_loader_instance_bound()`; `xud_physical.c` reads the six instance-table rows (`vkEnumeratePhysicalDevices`, the three `*2` caps getters, `vkGetPhysicalDeviceQueueFamilyProperties2`, `vkEnumerateDeviceExtensionProperties`) with the check-before-you-use rule kept — a row the driver did not answer stays NULL and the layer answers `XUD_UNSUPPORTED` rather than calling through it; `xud_device.c` calls `loader->instanceTable.vkCreateDevice` then `xud_loader_bind_device()`, and its guard/teardown rows (`vkGetDeviceQueue`, `vkDeviceWaitIdle`, `vkDestroyDevice`) come from `loader->deviceTable` behind `xud_loader_device_bound()`; `xud_diagnostics.c`'s messenger pair is now **table rows** (`instanceTable.vkCreateDebugUtilsMessengerEXT` / `…Destroy…`) instead of resolved-on-demand per call — the old reason for resolving them (a cached pointer into a dead layer) is subsumed by the re-bind rule; `xud_core.c`'s `xud_vulkan_supported()` no longer counts resolved slots, it opens a loader and trusts the open contract. `xud_instance_destroy()` **re-binds** the loader to the instance it is about to destroy when it is not already bound to it (a second instance on one loader leaves the loader bound to the first, and its rows would be used to destroy the second); the re-bind is skipped for a handle-less synthetic instance, which stays a free-only teardown with no driver call.

  **Bar met**: gcc **and** clang `-fsyntax-only -std=c11 -Wall -Wextra -Wpedantic -Werror` clean over the whole tree (clang also over `src/external/volk.c`); `ninja -C build` green; headless **472 checks / 0 failures** (was 465), `XUD_GPU_TESTS=1` **623 / 0** (was 615) and validation-clean with `VK_INSTANCE_LAYERS` unset **and** forced to `VK_LAYER_KHRONOS_validation` (the only `ERROR/validation` line in either run is still the diagnostics layer's own synthetic headless probe); `-DXUD_SANITIZERS=ON` (`build-san`) green both ways (472 / 623, no report); `./scripts/format.sh --check` clean; a scratch TU that includes only `xud.h` still compiles under the project flags, so user code still needs no `volk.h`, no `VK_NO_PROTOTYPES` and no Vulkan header. **Honest boundary**: the loader wrapper is per-context but Volk's globals are still process-wide, so two contexts each need their own `XudLoader` (they do — `XudContext` owns one) and `volkFinalize()` from one still clears the other's globals; nothing calls `volkLoadInstance`/`volkLoadDevice`/`volkInitializeCustom` anywhere in the tree (the tables are filled through the `*Table` forms only), and nothing resolves a **device-level** entry point through `vkGetDeviceProcAddr` yet — that stays the renderer's job.

## Steps complete (S6 Device + queues — S6.1–S6.4 all done, landed as one commit: the pure half S6.2 met no separate bar before the driver half S6.3 the same day, so the intermediate tree state never existed and cannot be re-verified)

- [x] ~~S6.1 `src/xud/xud_device.h` — the device handle + `XudQueue` + the layer's contract~~ — **done 2026-10-18**: internal module header like `xud_loader.h`/`xud_physical.h` (tests include it, user code includes `xud.h` only), pulls `xud.h`, `xud_loader.h`, `xud_instance.h`, `xud_physical.h`, `xud_vulkan.h`, `<vulkan/vulkan.h>` so it is includable on its own. `XudDevice` = the `VkDevice` handle + the `VkDeviceQueueCreateInfo` row the driver was handed + the `VkPhysicalDeviceFeatures` it was handed + the enabled-extension names it was handed (fixed `XUD_DEVICE_MAX_DEVICE_EXTENSIONS` 8 × `VK_MAX_EXTENSION_NAME_SIZE` storage, no heap member — unlike the physical layer's caps, this layer records nothing about the driver, it hands things to it) + the two `XudQueue` records it resolved. `XudQueue` is a **record**, not an owner: grepping `vkDestroyQueue` across `/usr/include/vulkan/` is zero hits, so there is no queue teardown rung. Queue roles `XUD_QUEUE_ROLE_GRAPHICS`/`XUD_QUEUE_ROLE_PRESENT` are a library decision (there is no `VK_QUEUE_PRESENT_BIT`, see the S5.2 grep). `XUD_DEVICE_QUEUE_MAX_COUNT` 2u is the ceiling on queues asked for from the one chosen family; the priority bounds are documented, **not** asserted — C11 `static_assert` needs an integer constant expression and both compilers rejected the float form (gcc `expression in static assertion is not an integer constant expression`, clang `folding it to a constant is a GNU extension`). The file-top records the header facts that decide the shapes: `PFN_vkCreateDevice` takes **four** arguments (physical device first), `PFN_vkDestroyDevice` and `PFN_vkGetDeviceQueue` return **void**, `PFN_vkDeviceWaitIdle` returns `VkResult`, `vkGetDeviceProcAddr` is not slotted or called here.

- [x] ~~S6.2 The pure half — no driver call~~ — **done 2026-10-18**: `xud_device_fill_queue_create_info` (stamp `VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO`, `pNext` NULL, `flags` 0, the family index from `caps.queueFamilyIndex`, `queueCount = min(family row, XUD_DEVICE_QUEUE_MAX_COUNT)`, `pQueuePriorities` → the caller's array), `xud_device_select_extensions` (the one wanted device-level name today, `VK_KHR_SWAPCHAIN`, enabled only if the physical layer's recorded list advertises it — the check-before-you-use rule at the device level), `xud_device_fill_create_info` (stamp + `pNext` NULL + one queue row + the legacy `enabledLayerCount`/`ppEnabledLayerNames` written as 0/NULL because the headers say they are unused and instance-level layers belong to `VkInstanceCreateInfo`, the names copied into caller-owned `nameSlots` with the tail NULLed, `pEnabledFeatures` → the struct's own feature record), `xud_device_queue_get` (pure lookup of what `xud_device_open()` resolved, role-range + no-handle + unresolved-slot + family/index-mismatch rules, leaves the out-record alone on every rejection).

- [x] ~~S6.3 The driver half — `xud_device_open` / `xud_device_wait_idle` / `xud_device_close`~~ — **done 2026-10-18**: `xud_device_open` binds the four device slots through the live `VkInstance` (all four were already in the table since S2 — `XUD_LOADER_SLOT_COUNT` stays 20), checks each nil before its first call, selects the extensions, asks `xud_physical_record_features()` for the feature record (never `caps.features.features` verbatim — that would enable everything the driver happens to have), builds the row from the recorded caps, calls `loader->createDevice(physical->physicalDevice, &createInfo, NULL, &device->device)`, resolves both role queues through `loader->getDeviceQueue` with the zero-state-before / check-after discipline that call needs (it returns `void`), and `goto fail` destroys a live device before freeing. `xud_device_wait_idle` is the teardown guard (the plan's `xud_wait_idle`, named after the layer per the repo rule). `xud_device_close` is guard → destroy → free, and a **lost** device is still destroyed (skipping `vkDestroyDevice` would leak an object the driver still holds) while `XUD_LOST` from the guard is still propagated.

- [x] ~~S6.4 `CMakeLists.txt` + `tests/xud_test.c` + docs~~ — **done 2026-10-18**: the TU joins `add_library(xud STATIC ...)` (**7** objects, `ar -t build/libxud.a` lists `xud_device.c.o`), the comment above the target names the device layer's four calls as reached only through the table; `tests/xud_test.c` gains `xud_test_device_pure()` (headless, synthetic caps + a synthetic live device: the row/extension/create-info assembly rules incl. the `XUD_OUT_OF_RESOURCES` extension-storage overflow, the queue lookup's four rejection arms, every argument guard of the three driver functions reached without a driver, the unbound-slot state of a fresh loader, a handleless guard answering `XUD_ALREADY` and a handleless close answering `XUD_OK` with no driver call) and `xud_test_device_gpu(loader, physical, instance)` called from **inside** `xud_test_gpu_physical()` before the physical device is closed (a `VkDevice` is created from a `VkPhysicalDevice` handle, so its teardown must happen while its owner is alive — the same rule the physical and diagnostics tests follow); AGENTS.md gains the *Verified device-layer facts* block, the *Present today* row names the pair as both-halves-in-the-archive, the loader-slot bullet records that the four device names needed **no** new slot, and the file-tops of `xud.h`, `xud_core.h` and `hello_context.c` say exactly what is and is not wired.

- [x] ~~The context does **not** own the device layer yet~~ — **deliberately left for S8 (2026-10-18)**: `xud_core.h`'s `device` field stays a `void *` sentinel and `xud_init_context` still answers `XUD_UNSUPPORTED` for valid args. The plan's S8 is the ownership + teardown step (loader/instance/device/surface/queues, destroy order device → surface → instance), and this step is the layer itself; the three file-tops that could read as if the context called it were corrected to say the layer exists and is tested but nothing calls it.

## Steps complete (S5 Physical device — S5.1–S5.8 all done; the driver step S5.4 carried S5.5/S5.6/S5.7/S5.8 with it)

- [x] ~~S5.4 The driver half: `xud_physical_query_devices` + `xud_physical_open`~~ — **done 2026-10-18**, see *Done* below: `xud_physical_query_devices` (`vkEnumeratePhysicalDevices` through its slot, the same doubling-capacity shape as the other input/output-count queries, probe-recorded **1 device on lavvk here**) and `xud_physical_open` (bind the six physical slots through the live `VkInstance`, one allocation for the struct + its ~1.7 KB caps, enumerate → select → the four caps queries → the device-extension list → the queue-family rule → the floor check, `goto fail` frees in reverse creation order and destroys nothing). The three void caps getters get the zero-state-before-the-call discipline the item named, and the validation layer turned one of those zero states into a hard error: an unstamped `VkQueueFamilyProperties2` row answers `pQueueFamilyProperties[i].sType must be VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2` for **every row of the capacity passed in**, so the queue-family query stamps the whole array before the call. **Note on the split**: S5.3 met its bar
      earlier the same day but was never committed (the commit before this one is S5.2's header), so the pure
      half lands in this commit alongside the driver half — the intermediate tree state (pure half only, the
      CMake stopgap compiling the TU into the test binary, 339 headless checks) no longer exists and cannot be
      re-verified, so it was not committed as its own step; the log above keeps both steps' notes separate.

- [x] ~~S5.5 `xud_core.h`/`xud_core.c`: the `void *physicalDevice` sentinel becomes `XudPhysicalDevice *physicalDevice`~~ — **done with S5.4 (2026-10-18)**: the field is typed, `xud_init_context` calls `xud_physical_open(&context->physicalDevice, context->loader, context->instance->instance)` between `xud_instance_create` and the `XUD_UNSUPPORTED` return (the comment says what `XUD_UNSUPPORTED` means here: loader + instance fine, no device enumerable / a nil PFN slot / the caps refusing the floor — the honest boundary stays until S6), and `xud_close_context` frees it in reverse creation order **before** `xud_instance_destroy`, free-only, the comment naming that since every other rung of that ladder is a driver call.

- [x] ~~S5.6 `CMakeLists.txt`: add `src/xud/xud_physical.c` to `add_library(xud STATIC ...)`~~ — **done with S5.4 (2026-10-18)**: the stopgap that compiled the TU into the test binary is gone (both halves are defined now, the header's contract is complete), the archive lists **6** objects with `xud_physical.c.o` among them, the comment above the target names the physical layer's driver calls as reached only through the table, still no `-lvulkan`, no `-ldl`.

- [x] ~~S5.7 `tests/xud_test.c`: `xud_test_gpu_physical` + the caps-storage overflow check~~ — **done with S5.4 (2026-10-18)**: `xud_test_gpu_physical(loader, instance)` runs **inside** `xud_test_gpu_instance()` before the instance is destroyed (the physical device is owned by the instance, so its teardown must happen while the owner is alive — same rule as the diagnostics messenger's teardown inside `xud_instance_destroy()`), and checks enumerate → pick → record on lavvk: device count, real `apiVersion` ≥ floor, `VK_KHR_SWAPCHAIN` among the advertised **device** extensions, a graphics family found, the ∩ rule against the driver's own answer, and the teardown. `xud_test_physical_negative()` (headless) covers the argument rules the driver half states without a driver: a nil table, a NULL out-param, a NULL out-handle, a NULL-in-handling close, and the caps' `pNext` rule. The `XUD_OUT_OF_RESOURCES` overflow arm of `xud_physical_select_queue_family` is exercised headless in `xud_test_physical_pure` (the synthetic two-family row); the device-extension overflow is `XUD_OUT_OF_MEMORY` from the doubling query, which the headless run cannot reach without a driver — written into the honest boundary rather than faked.

- [x] ~~S5.8 Docs~~ — **done with S5.4 (2026-10-18)**: AGENTS.md gains the *Verified physical-layer driver facts* block with the probed numbers, the *Present today* row names the pair as both-halves-in-the-archive, the toolchain table is corrected to the headers/loader/validation versions this step found on the machine (1.4.357 → **1.4.363**), the loader-slot bullet says **20** slots / seven physical names, and the file-tops of `xud_physical.h`/`.c`, `xud_core.h`/`.c`, `xud.h` and `hello_context.c` name the layer and the archive state exactly as the tree has it.

- [x] ~~S5.2 `src/xud/xud_physical.h` — the physical handle + `XudDeviceCaps` + the layer's contract~~ — **done 2026-10-18**, see *Done* below: `XudPhysicalDevice` = the `VkPhysicalDevice` handle + an embedded-by-value `XudDeviceCaps` (the three `*2` structs + the chosen queue row + the one heap member, the advertised **device**-extension list), the contract split the item's way — driver `xud_physical_query_devices` / `xud_physical_open`, pure `xud_physical_select` / `xud_physical_record_features` / `xud_physical_select_queue_family` / `xud_physical_record_api_version` — plus `xud_physical_close`, which exists from the start because S5.5's teardown ladder needs its rung: grepping these headers finds **no `vkDestroyPhysicalDevice`** (a physical device is a query result, nothing the driver allocated for us), so that rung frees memory and calls nothing, which is also why it takes no `XudLoader *` unlike every other close in the tree. Declarations only — `xud_physical.c` is S5.3's/S5.4's and joins the archive at S5.6, so the archive is unchanged and `XUD_LOADER_SLOT_COUNT` stays 19 (S5.1 already slotted all six physical names). Two greps this step settled change the plan's wording and are written into S5.3/S5.7 below: **`VK_QUEUE_PRESENT_BIT` does not exist** in these headers (presentation is a per-window-system query — GLFW's `glfwGetPhysicalDevicePresentationSupport` at S7 — so the queue rule this layer can enforce is the **graphics** bit and the present half of AGENTS.md's one-family rule is refused at S7), and **`VkPhysicalDeviceArchitectureInfo` is not in these headers at all**, so the only core fields a discrete-preference policy could rank on are `deviceType` + `vendorID`/`deviceName`/`pipelineCacheUUID` and ranking stays a decision, not code.

- [x] ~~S5.3 The pure half — no driver call, so the headless run covers it completely: `xud_physical_select` (given the enumerated handles and their `VkPhysicalDeviceProperties`, pick one; policy stays "first" for now, and the reason a better policy is a decision rather than code is that the architecture-id chained struct is **not** in these headers (grep `VkPhysicalDeviceArchitectureInfo` → zero hits), so all a discrete-preference policy could sort on is core `VkPhysicalDeviceProperties.deviceType` (`VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU`/`INTEGRATED_GPU`/`VIRTUAL_GPU`/`CPU`, verified at 2233–2238) plus `vendorID`/`deviceName`/`pipelineCacheUUID`); `xud_physical_record_features` (wanted ∩ the driver's `VkPhysicalDeviceFeatures2.features`, an unverified feature stays 0 and is never forced on — this is the struct S6 puts into `VkDeviceCreateInfo`); `xud_physical_select_queue_family` (a family carrying **`VK_QUEUE_GRAPHICS_BIT`** — the plan's "both graphics and present" wording is corrected by the S5.2 grep: `VK_QUEUE_PRESENT_BIT` does not exist in these headers, so the present half of AGENTS.md's one-family rule is GLFW's `glfwGetPhysicalDevicePresentationSupport` check at S7, and a graphics-only family is chosen here and refused there; transfer/compute/granularity recorded not required, record once and never re-query); `xud_physical_record_api_version` (read `properties.apiVersion` back, compare with `XUD_INSTANCE_API_FLOOR`, answer which of the *2 paths are core at that version and which are extension-at-the-floor — the whole point of the floor decision)~~ — **done 2026-10-18**, see *Done* below: `src/xud/xud_physical.c` lands with the five definitions that need no driver (`xud_physical_select`, `xud_physical_record_features`, `xud_physical_select_queue_family`, `xud_physical_record_api_version`, and `xud_physical_close`, free-only — it lands here rather than at S5.4 because the sanitizer run of the caps teardown is this step's bar), and **not** with the two that do (`xud_physical_query_devices`, `xud_physical_open` — S5.4's, declared by the header and defined nowhere, which is legal today because this TU is not in the archive yet and nothing calls them). **Done as**: `xud_physical_select` keeps the "first in driver order" policy and says in a comment why nothing ranks yet (the architecture-id chained struct is absent from these headers, so the only core ranking fields are `deviceType` + `vendorID`/`deviceName`/`pipelineCacheUUID` — enough to prefer a discrete GPU, not enough to identify one, and preferring one is a decision, not code); `xud_physical_record_features` intersects a file-scope `static const VkPhysicalDeviceFeatures XUD_PHYSICAL_WANTED_FEATURES` with the driver's `caps->features.features` through one helper `xud_physical_and_feature` (VK_TRUE only when both sides are VK_TRUE, so a driver's non-0/non-1 answer normalises to VK_TRUE/VK_FALSE and the struct S6 hands to `VkDeviceCreateInfo` is exactly 0/1), writes every field in struct order, and leaves the driver's verbatim answer in `caps->features` untouched; the wanted list is **three names**, each with a reason written beside it — `robustBufferAccess` (the only feature whose absence is a correctness hazard: without it, out-of-range index reads are undefined rather than clamped, and the batch layer will not bounds-check every draw), `samplerAnisotropy` (ground truth for AGENTS.md's "check before you use" rule — widely absent, so the sampler variant has to gate on the caps), `wideLines` (raylib's `DrawLine3D` width; absent on most Vulkan drivers, so lines fall back to width 1 rather than failing); deliberately **not** wanted today: `fillModeNonSolid` (wireframe, tempting for raylib's `DrawCubeWires` but it is a pipeline variant this layer does not own), `depthClamp`/`depthBiasClamp`, `multiViewport`, the whole sparse/texture-compression family, and `VK_KHR_*` feature names (device extensions are recorded by S5.4 and their features are a chained struct this struct cannot carry). `xud_physical_select_queue_family` implements the graphics rule the header states (first family in driver order whose `queueFlags` carry `VK_QUEUE_GRAPHICS_BIT`, the whole `VkQueueFamilyProperties2` row copied once into the out-param, `sType`/`pNext` normalised on the copy), answers `XUD_PHYSICAL_NO_QUEUE_FAMILY` in the index and a **zeroed** row on refusal (a row that still carried the rejected family's flags would be read by S6 as a queue to create), and never requires transfer/compute/granularity. `xud_physical_record_api_version` reads `caps->properties.properties.apiVersion` into `caps->apiVersion` verbatim — even when the check fails, so a rejection can be diagnosed — and answers `XUD_UNSUPPORTED` below `XUD_INSTANCE_API_FLOOR`. `xud_physical_close` frees the caps' one heap member then the struct, clears the array pointer and both counts, answers `XUD_ALREADY` on a NULL-in-handling pointer, and calls no driver function. Six `static_assert`s, each with a message (clang 22 rejects the message-less form here): `VK_QUEUE_GRAPHICS_BIT` is non-zero, is exactly one bit (`(b & (b-1)) == 0`, the check assumes it), is distinct from `VK_QUEUE_TRANSFER_BIT` (recorded, never required), `VK_FALSE == 0u`/`VK_TRUE == 1u` (the normalisation the feature helper writes), and `VK_API_VERSION_1_0 <= VK_API_VERSION_1_4` (the floor comparison below needs the encoding to be ordered, oldest first — `VK_MAKE_API_VERSION(0, major, minor, patch)` puts major above minor, verified here). **One assert removed on the way** (found by clang, not by the compiler): `sizeof(XUD_PHYSICAL_WANTED_FEATURES) == sizeof(VkPhysicalDeviceFeatures)` looked like the item's "wanted ∩ driver" guarantee but both spellings are the **same typedef** — `VkPhysicalDeviceFeatures2` is the wrapper, its `.features` member is the core struct — so the assert was a tautology; the real check is the designated initializers, which fail at compile time if the wanted struct is ever spelled as the wrapper. **The member-path trap this step found** (and the one thing that broke the build today): from the caps the apiVersion is **two** hops, `caps->properties.properties.apiVersion` (the caps field is itself named `properties`), from a bare `VkPhysicalDeviceProperties2` variable it is **one**, `props[i].properties.deviceType` — the test fixture wrote the two-hop form against a one-hop variable and gcc answered `'VkPhysicalDeviceProperties' has no member named 'properties'; did you mean 'sparseProperties'?`. `tests/xud_test.c` gains `xud_test_physical_pure` (headless, synthetic-everything: the null/empty guards incl. a non-zero count with a NULL array, selection incl. a three-device list whose `deviceType` differs and must not reorder, the ∩ rule in all four shapes plus a non-normalised driver answer, the queue rule incl. a compute-only family that must not be chosen and a graphics-only family that must be, the sentinel + zeroed row on refusal, `apiVersion` at exactly the floor and one below it, and `xud_physical_close` on a heap caps struct with and without the extension array) plus the `#include "xud_physical.h"` and the `main()` call, and `CMakeLists.txt` compiles `src/xud/xud_physical.c` **into the `xud_test` executable** (not the archive — S5.6's step adds the archive line when the contract is complete) with a comment saying why: the pure half exists, the driver half does not, so the functions are uncallable from `libxud.a` today and the only way to test them at all is to build this TU into the test binary. Bar met: gcc **and** clang `-fsyntax-only -std=c11 -Wall -Wextra -Wpedantic -Werror` clean over the whole tree; `ninja -C build` green; `./build/xud_test` headless **339 checks / 0 failures** (was 281 at S5.2 — this step adds 58) and **401 / 0** with `XUD_GPU_TESTS=1` (was 343); `-DXUD_SANITIZERS=ON` (`build-san`) green both ways, which is the point of landing `xud_physical_close` here (the heap caps struct + its extension array are freed under ASan with no driver involved); the archive is unchanged (still 5 objects, `ar -t build/libxud.a` does **not** list `xud_physical.c.o` — S5.6's job) and the link is still Vulkan-free (`ldd build/xud_test` lists no `libvulkan`/`libdl`, `nm -u build/xud_test` lists no `vk*`).

- [x] ~~S5.1 Loader table grows by the **one** slot the physical layer needs~~ — **done 2026-10-18**, see *Done* below: `vkEnumeratePhysicalDevices`, `getPhysicalDeviceProperties` (the non-2 form), `getPhysicalDeviceFeatures2`, `getPhysicalDeviceQueueFamilyProperties2` and `enumerateDeviceExtensionProperties` are already slots of the instance-level tier; only `PFN_vkGetPhysicalDeviceProperties2` was missing (verified: declared at vulkan_core.h:6179, with a `...KHR` twin at 10361 that is what `VK_KHR_get_physical_device_properties2` — already enabled on the instance by `xud_instance.c` — makes callable at the 1.0 floor). Add that one field, bump `XUD_LOADER_SLOT_COUNT` 18 → 19, keep the "grep -c, keep exact" comment honest, and do **not** resolve the *2 queries on demand like the messenger pair. **Done as**: one field added to `XudLoader`, in the instance-level tier directly after the non-2 form it pairs with, each labelled with what it is for (the non-2 form is the 1.0-core one the selection policy needs with no extension in hand; the *2 form is core in 1.1 and reachable at the floor only through that extension); `XUD_LOADER_SLOT_COUNT` 18 → 19 and the comment under it still true — `awk '/^typedef struct XudLoader/,/^} XudLoader;/' xud_loader.h | grep -c PFN_vk` answers **19**. The *Not slotted* comment now records why the *2 physical queries **are** slotted while the messenger pair is not: probed here today (dlopen only, no link dependency, `VK_INSTANCE_LAYERS` unset) through a **bare** instance — `apiVersion = VK_API_VERSION_1_0`, no extension, no layer — and through the instance `xud_instance.c` creates, `vkGetInstanceProcAddr` answers non-NULL for all six names the physical layer needs (`vkEnumeratePhysicalDevices`, `vkGetPhysicalDeviceProperties`, `vkGetPhysicalDeviceProperties2`, `vkGetPhysicalDeviceFeatures2`, `vkGetPhysicalDeviceQueueFamilyProperties2`, `vkEnumerateDeviceExtensionProperties`) through either handle, and NULL for all six with `VK_NULL_HANDLE` in hand — which is why they are instance-level slots and not global ones, and why they are cached (they name the driver's own code, not a layer instantiated per instance, so a slot stays valid across an instance recreation, which a messenger pointer would not). lavvk answers the *2 forms even with `VK_KHR_get_physical_device_properties2` **not** enabled on the bare instance, so the slot fills either way here; a driver that gates them answers NULL, and the layer that binds such a slot reports the nil slot rather than calling through it (the same check `xud_instance.c` applies to `createInstance`). The instance-tier comment now states that verified handle rule instead of claiming only `xud_instance.c` fills the tier. Nothing else touched: `XUD_LOADER_GLOBAL_SLOT_COUNT` stays 3 (the new slot is not global-level, so `xud_vulkan_supported()`'s check is unchanged), `xud_loader.c` is untouched (it static_asserts only the three global PFN types; the physical one is asserted where S5.4 binds it), no test or example change (S5.7's job). Bar met: gcc **and** clang `-fsyntax-only -std=c11 -Wall -Wextra -Wpedantic -Werror` clean over the whole tree — every `.c` in `src/xud`, `tests/xud_test.c`, `examples/hello_context.c`, plus a TU that includes only `xud_loader.h` — in all three flag sets the build uses (`-g -DXUD_DEBUG=1`, `-O2 -DXUD_DEBUG=0 -DNDEBUG`, `-fsanitize=address,undefined -g`); `ninja -C build` rebuilds the archive and both executables, `./build/xud_test` green headless (**281/0**) and with `XUD_GPU_TESTS=1` (**343/0**) — unchanged from S4, this step adds no call site and no test; `-DXUD_SANITIZERS=ON` (`build-san`) green; link still Vulkan-free (`ldd build/xud_test` lists no `libvulkan`/`libdl`, `nm -u` lists no `vk*`).

## Steps complete (Scaffold + Vulkan bring-up)

- [x] ~~S1 Repo skeleton~~ — **done 2026-10-16**, see *Done* below.
- [x] ~~S2 Loader bring-up~~ — **done 2026-10-16**, see *Done* below.
- [x] ~~S3 Instance~~ — **done 2026-10-16**, see *Done* below.
- [x] ~~**S4 Diagnostics layer**~~ — **done 2026-10-16**, see *Done* below: validation layers, debug-gated (`XUD_VALIDATE`): `vkEnumerateInstanceLayerProperties`, request **everything advertised** (never a hardcoded name), hand the selected layers + `VkValidationFeaturesEXT` + the wanted extensions to the instance layer, and install a `VK_EXT_debug_utils` messenger for the log side. Note: `VK_VALIDATE_*` core features are **not in these headers** (SDK-only) — only `VkValidationFeaturesEXT` on the `pNext` chain is.

      - [x] ~~S4.1 `xud_diagnostics.c` compiles clean~~ — **done 2026-10-16**: both fixes applied in `xud_diagnostics.c`. `xud_diagnostics_close()` now calls `vkDestroyDebugUtilsMessengerEXT` with its **three** arguments — `PFN_vkDestroyDebugUtilsMessengerEXT` in these headers is `(VkInstance, VkDebugUtilsMessengerEXT, const VkAllocationCallbacks *)`, so the missing tail is `pAllocator = NULL`, the same default allocator the create call above already passes (the first parameter really is `VkInstance` here, not `VkDevice`: the extension's instance-level form is what Vulkan-Headers 1.4.357 declares, which is why the borrowed-instance rule in the header comment is right). `xud_diagnostics_message_callback` now prints the id with `%d` against an `(int32_t)0` fallback arm — `messageIdNumber` is `int32_t` in `VkDebugUtilsMessengerCallbackDataEXT`, the two arms of `?:` must agree in type, and `0u` there is a hard `-Werror=sign-compare` error, not a warning. Verified: `gcc` **and** `clang` `-fsyntax-only -std=c11 -Wall -Wextra -Wpedantic -Werror` clean on `src/xud/xud_diagnostics.c` **and** on a TU that includes only `xud_diagnostics.h`, in all three flag sets the build uses (`-g -DXUD_DEBUG=1`, `-O2 -DXUD_DEBUG=0 -DNDEBUG`, `-fsanitize=address,undefined -g`). Still not built, not linked, not tested — see S4.2–S4.7.
      - [x] ~~S4.2 `xud_instance.c` catches up with its own header~~ — **done 2026-10-16**: the three definitions now match `xud_instance.h`. `xud_instance_fill_create_info` takes `const XudDiagnostics *` (pNext → `&diagnostics->validationFeatures` when present, NULL when not; `enabledLayerCount`/`ppEnabledLayerNames` taken from the diagnostics struct, 0/NULL when it is absent — the old "the validation layers are not here yet" comment is gone, that IS this step's job now; `XUD_INVALID_ARG` also when a diagnostics struct's layer count does not fit its own storage). `xud_instance_create` takes `XudDiagnostics *` and asks **the accessor** (`xud_diagnostics_wanted_extensions`) for the diagnostics wanted-extension list it feeds to `xud_instance_select_extensions` — the header's `extern` array of the same list is defined nowhere, so reading it here would be an undefined symbol at link time, not a compile error (S4.3 kills the declaration). `xud_instance_destroy` takes it too and destroys the messenger **first, through the `VkInstance` that is about to go away** (new static helper `xud_instance_close_messenger`: resolved on demand through `xud_loader_resolve` into a local `XudPfnBits`, never cached in a table slot — xud_loader.h's rule), then clears `diagnostics->messenger`/`->instance` and does **not** free the struct (this layer only holds a copy of that pointer; `xud_close_context()` frees it via `xud_diagnostics_close()`), so the header's "A diagnostics struct with no messenger is just freed" sentence was rewritten to say exactly that; a messenger belonging to a different `VkInstance` than the one being destroyed is `XUD_INVALID_ARG`. The two `static_assert`s that reference `XUD_INSTANCE_WANTED_COUNT` moved **below** the `#define` (verified with them above it: gcc 16.2.1 reports both `'XUD_INSTANCE_WANTED_COUNT' undeclared here (not in a function)` and `expression in static assertion is not an integer`), and one more `static_assert` was added for `PFN_vkDestroyDebugUtilsMessengerEXT` now that this file calls through it. Verified: `gcc` **and** `clang` `-fsyntax-only -std=c11 -Wall -Wextra -Wpedantic -Werror` clean on `src/xud/xud_instance.c` alone in all three flag sets (`-g -DXUD_DEBUG=1`, `-O2 -DXUD_DEBUG=0 -DNDEBUG`, `-fsanitize=address,undefined -g`); the whole tree still fails, now only at the two S4.4 core call sites.
      - [x] ~~S4.3 Pick **one** mechanism for the diagnostics wanted-extension list~~ — **done 2026-10-16**: the accessor stays, the extern dies. `xud_instance.h` no longer declares `extern const char *const XUD_INSTANCE_DIAGNOSTICS_WANTED_EXTENSIONS[]` (defined nowhere — reading it would have been an undefined symbol at link time, not a compile error) and no longer declares a second size macro for that list: `xud_instance_select_extensions` now takes `const char *const extraNames[XUD_DIAGNOSTICS_WANTED_MAX]`, the diagnostics layer's own cap, which the header sees through the `#include "xud_diagnostics.h"` it already needed for `xud_instance_fill_create_info`'s new parameter. The four uses in `xud_instance.c` follow (the two function signatures + the two local `diagnosticsWanted` arrays). In `xud_instance.c` the `XUD_INSTANCE_DIAGNOSTICS_WANTED_MAX == 2u` assert is gone — it asserted a **copy** of the diagnostics layer's number, not a fact about this layer — replaced by `XUD_DIAGNOSTICS_WANTED_MAX <= XUD_MAX_INSTANCE_EXTENSIONS` next to the union-fit assert, which now uses the same single macro; one macro for one number cannot drift, so the arrays and the accessor's out-array cannot disagree. Same step, docs bug fixed in `xud_diagnostics.c`: its file-top claimed `VK_EXT_debug_utils` **and** `VK_EXT_validation_features` were both advertised at instance level. Re-probed today (dlopen-only, no link dependency): **26** instance-level extensions advertised, `VK_EXT_debug_utils` among them, `VK_EXT_validation_features` **not**, with or without `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` in the environment; requesting it in `ppEnabledExtensionNames` answers `VK_INCOMPLETE` (-7), while `VkValidationFeaturesEXT` on the `pNext` chain **is** accepted (`VK_SUCCESS`, with and without features set, even unrequested) — which is exactly the gate `xud_instance_all_names_enabled` now applies. Verified: gcc **and** clang `-fsyntax-only -std=c11 -Wall -Wextra -Wpedantic -Werror` clean on `src/xud/xud_instance.c`, `src/xud/xud_diagnostics.c`, and on TUs including only `xud_instance.h` / only `xud_diagnostics.h`, in all three flag sets (`-g -DXUD_DEBUG=1`, `-O2 -DXUD_DEBUG=0 -DNDEBUG`, `-fsanitize=address,undefined -g`); `grep -rn XUD_INSTANCE_DIAGNOSTICS_WANTED_MAX` over the tree is empty; the tree still fails only at the two S4.4 core call sites (plus `tests/xud_test.c`, S4.6's job — see *Uncommitted state*).
      - [x] ~~S4.4 `xud_core.h`/`xud_core.c`: add the `diagnostics` field to `XudContext`, call `xud_diagnostics_open()` between loader-open and instance-create (gated on `XUD_VALIDATE`, NULL in a release build), destroy the messenger **before** the instance (the extension's own rule: the owning `VkInstance` must still be alive), then `xud_diagnostics_close()` after `xud_instance_destroy()`~~ — **done 2026-10-16**: `xud_core.h` includes `xud_diagnostics.h` explicitly (it had been seeing the type only through `xud_instance.h`) and says what the field means in a release build (`NULL` — "no layer requested, no validation request, no messenger"). `xud_init_context` opens the diagnostics struct between loader-open and instance-create behind `context->validate != 0u`, the `XUD_VALIDATE` snapshot `xud_context_create` took, so a release build carries no diagnostics struct at all and teardown is a no-op on `NULL`. **One thing beyond the item text, and it has a reason**: the messenger is created in `xud_init_context` **after** `xud_instance_create` — `vkCreateDebugUtilsMessengerEXT` is instance-level, so it needs a live `VkInstance` and cannot be created inside `xud_diagnostics_open()`; `XUD_UNSUPPORTED` from it is tolerated (that instance has no `VK_EXT_debug_utils` enabled, so there is no log side for us and the layers are still enabled — the honest "no log side" answer, not a failure), every other failure goes to the `goto fail` ladder. Teardown is `xud_instance_destroy(..., diagnostics)` → `xud_diagnostics_close` → `xud_loader_close`: the messenger dies inside `xud_instance_destroy`, through the instance that owns it and while that instance is still alive (S4.2's `xud_instance_close_messenger`), and `xud_diagnostics_close` then finds `messenger`/`instance` already cleared, so it frees the struct with no driver call. Verified here: `xud_core.c` compiles clean under **gcc and clang** with the project flags; a scratch TU that mirrors the sequence (it includes `xud_core.h`, so it is not a user-facing shape) reports 3 layers selected (`VK_LAYER_MESA_device_select`, `VK_LAYER_MESA_anti_lag`, `VK_LAYER_KHRONOS_validation`), 4 extensions enabled (the three instance-wanted ones + `VK_EXT_debug_utils`), messenger handle non-`VK_NULL_HANDLE`, `instance_destroy -> XUD_OK` with `messenger`/`instance` cleared after it, `diagnostics_close -> XUD_OK`, `loader_close -> XUD_OK`; `hello_context` links against the five library sources and still answers `XUD_UNSUPPORTED` (nothing below the instance is reachable) and is **leak-free under `-fsanitize=address,undefined`** in both the debug config (diagnostics open) and the release config (`-DXUD_DEBUG=OFF`, diagnostics `NULL`), with `VK_INSTANCE_LAYERS` unset, set to an advertised layer, and set to a name that does not exist. Still not committed: `tests/xud_test.c` (S4.6) is the remaining compile failure, so the whole-tree build stays red.
      - [x] ~~S4.5 `CMakeLists.txt`: add `src/xud/xud_diagnostics.c` to the `xud` archive~~ — **done 2026-10-16**: it is now the second source of `add_library(xud STATIC ...)` (after `xud_core.c`, before `xud_loader.c`), and the comment above the target names the diagnostics layer's three driver calls (`vkEnumerateInstanceLayerProperties`, `vkCreateDebugUtilsMessengerEXT`, `vkDestroyDebugUtilsMessengerEXT`) as reached **only** through the loader's table, with the two messenger entry points resolved on demand through a live `VkInstance` rather than cached in a slot — so the archive's "no Vulkan symbol at link time" claim still holds and says why. Verified here: `cmake -B build -G Ninja` + `ninja -C build xud` compiles **5** objects and `ar -t build/libxud.a` lists `xud_diagnostics.c.o`; `nm -P build/libxud.a` writes **no** undefined `vk*`/`glfw*` symbol; `ninja -C build hello_context` links against the new archive, `ldd build/hello_context` lists no `libvulkan`/`libdl`/`libglfw`, `nm -u build/hello_context` lists no `vk*`; the `-DXUD_SANITIZERS=ON` archive (`build-san`) builds the same 5 objects; gcc **and** clang `-fsyntax-only -std=c11 -Wall -Wextra -Wpedantic -Werror` are clean on `src/xud/xud_diagnostics.c` in all three flag sets the build uses (`-g -DXUD_DEBUG=1`, `-O2 -DXUD_DEBUG=0 -DNDEBUG`, `-fsanitize=address,undefined -g -fno-omit-frame-pointer`), and the whole-tree syntax check now fails at **one** TU, `tests/xud_test.c` (S4.6). Still not committed — S4.6/S4.7 remain, so the commit bar is not met (see *Uncommitted state*).
      - [x] ~~S4.6 `tests/xud_test.c`: zero diagnostics coverage~~ — **done 2026-10-16**: headless tests for the pure parts (`wanted_extensions`, `fill_validation_features`, `fill_messenger_create_info`, `select_layers` incl. the `XUD_OUT_OF_RESOURCES` overflow, `severity_name` incl. the NULL arms, and the callback driven directly with synthetic `VkDebugUtilsMessengerCallbackDataEXT` so the counters are checked with no driver); GPU-gated test for `open`/`create_messenger`/`close` and a run that must be **validation-layer-clean** (the `messagesReceived`/`errorMessages` counters are the failure signal, not just "didn't crash"). **Done as**: `xud_test_diagnostics_pure` (the wanted pair in order and at its own cap, the validation-feature request with nothing disabled, the messenger create-info severities/types + `pUserData` → the struct, `select_layers` incl. the overflow and the same-name/different-description rule, `severity_name` incl. the NULL arms, and the callback fed synthetic data — a NULL `callbackData` counts into a live struct and counts nothing when `pUserData` is NULL, there is no struct to count into) plus `xud_test_gpu_diagnostics` (query/open/create_messenger/close on a live driver: 3 layers selected, messenger non-`VK_NULL_HANDLE`, `instance`/`messagesReceived`/`errorMessages` at their zero state, and the whole GPU run asserted on `messagesReceived == 0`/`errorMessages == 0` as the validation-clean signal, not "didn't crash"). The instance tests also had to follow the signatures the diagnostics layer grew — S4.2 changed real arities, so the synthetic advertised lists now carry all three of the instance layer's own wanted names the way lavvk answers them, and the diagnostics pair lands at index 3/4 with create-info counts 4 and 5. Bar: headless **281 checks / 0 failures**, `XUD_GPU_TESTS=1` **343 / 0** with `VK_INSTANCE_LAYERS` unset and forced to `VK_LAYER_KHRONOS_validation`, `-DXUD_SANITIZERS=ON` green.
      - [x] ~~S4.7 Docs~~ — **done 2026-10-16**: `AGENTS.md` *Repo layout* "Present today" row and the *Verified instance bring-up* section must name `xud_diagnostics.h`/`.c` and record the loader facts this step verifies (layers advertised, messenger create/destroy, the pNext-chain probe result); `xud_core.h`'s file-top comment gains the diagnostics field; one dated `todo.md` note when the bar is met. **Done as**: the *Present today* row names the diagnostics pair, a new *Verified diagnostics bring-up (2026-10-16)* block records the facts (3 layers advertised, by name; the layer query has the same input/output `pCount` shape as the extension query; the two messenger entry points resolved on demand through a live `VkInstance`; `vkDestroyDebugUtilsMessengerEXT`'s three-argument, instance-level form; the callback's counting rule; and the deliberately-not-done `VkDebugUtilsMessengerCreateInfoEXT` chain that segfaults inside `vkCreateInstance`), and the *Verified instance bring-up* section's now-stale "validation layers are not requested yet" bullet is replaced by what happens with the diagnostics struct (4 extensions enabled, `VK_EXT_validation_features` not advertised here so the chain stays empty); `xud_core.h`'s file-top names the diagnostics field and its place in both orders, `xud.h`'s *Layer* note names the layer as internal (no public declaration — like the loader and the instance layers, user code includes `xud.h` only), and `hello_context.c`'s file-top says S4, not S3.

## Done

- 2026-10-18 — **the device layer, both halves, and the layer landing (S6, S6.1–S6.4 as one commit)**: `src/xud/xud_device.h` + `src/xud/xud_device.c` land and join the archive (**7** objects, `ar -t build/libxud.a` lists `xud_device.c.o`). **Header**: `XudDevice` owns the `VkDevice` handle, the `VkDeviceQueueCreateInfo` row the driver was handed, the `VkPhysicalDeviceFeatures` it was handed, the enabled-extension names it was handed (fixed `XUD_DEVICE_MAX_DEVICE_EXTENSIONS` 8 × `VK_MAX_EXTENSION_NAME_SIZE` storage, no heap member — this layer records nothing about the driver, unlike the physical layer's caps) and the two `XudQueue` records it resolved; `XudQueue` is a **record** of a queue (`VkQueue` + the family/index pair it came from), not an owner — grepping `vkDestroyQueue` across `/usr/include/vulkan/` is zero hits, so there is no queue teardown rung and `xud_device_close()` frees no queue. The layer is named `xud_device`, not the plan's `xud_device_queue`: a queue cannot exist without the device that owns it (`PFN_vkGetDeviceQueue` takes a `VkDevice`), and `XudDevice` is that owner. Queue roles (`XUD_QUEUE_ROLE_GRAPHICS`/`XUD_QUEUE_ROLE_PRESENT`) are a library decision, not Vulkan state — there is no `VK_QUEUE_PRESENT_BIT` (S5.2 grep), the roles are how the layer above asks for a queue, and on this machine both resolve to the same handle. `XUD_DEVICE_QUEUE_MAX_COUNT` 2u is the ceiling; the priority bounds are documented, **not** asserted — C11 `static_assert` needs an integer constant expression and both compilers rejected the float form (gcc `expression in static assertion is not an integer constant expression`, clang `folding it to a constant is a GNU extension`). **Header facts** (grepped in `vulkan_core.h` 1.4.363, no driver call): `VkDeviceQueueCreateInfo` at **3727** — the plan's `VkQueueCreateInfo` does not exist, grep of that name is zero hits — `VkDeviceCreateInfo` at **3736** with the two **legacy** members the headers themselves comment as unused (`enabledLayerCount`, `ppEnabledLayerNames`, written 0/NULL here because instance-level layers belong to `VkInstanceCreateInfo`); stamps `VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO = 2` / `VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO = 3` (asserted distinct and non-zero); `VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT = 0x00000001` is the only queue-flag bit here and stays off; PFN lines 4540 `vkGetDeviceProcAddr`, 4541 `vkCreateDevice` (**four** arguments, physical device first), 4542 `vkDestroyDevice` (**void**), 4547 `vkGetDeviceQueue` (**void**), 4550 `vkDeviceWaitIdle` (`VkResult`). **Pure half**: `xud_device_fill_queue_create_info` (stamped, `pNext` NULL, `flags` 0, family from `caps.queueFamilyIndex`, `queueCount = min(family row, ceiling)`, `pQueuePriorities` → the caller's array), `xud_device_select_extensions` (one wanted device-level name today, `VK_KHR_SWAPCHAIN`, enabled only if the physical layer's recorded list advertises it — the check-before-you-use rule at the device level, `XUD_OUT_OF_RESOURCES` on storage overflow), `xud_device_fill_create_info` (stamped, one queue row, names copied into caller-owned `nameSlots` with the tail NULLed, `pEnabledFeatures` → the struct's own record), `xud_device_queue_get` (pure lookup, four rejection arms, out-record left alone on every one). **Driver half**: `xud_device_open` binds the four device slots through the live `VkInstance` — **no new loader slot was needed**, all four were in the table since S2, `XUD_LOADER_SLOT_COUNT` stays **20** — probed through `xud_loader_resolve`: all four answer `XUD_OK` through a live `VkInstance` and `XUD_UNSUPPORTED` with `VK_NULL_HANDLE` in hand, so they are instance-level for resolution and cached (teardown calls through the slot, it does not resolve again); `vkGetDeviceProcAddr` is the one device-level name **not** slotted and not called here (it needs the `VkDevice` itself, the renderer layer's job). The feature record handed to `VkDeviceCreateInfo` comes from `xud_physical_record_features()` — the **intersection**, never `caps.features.features` verbatim. `loader->createDevice(physical->physicalDevice, &createInfo, NULL, &device->device)`; both role queues resolved with the zero-state-before / check-after discipline `PFN_vkGetDeviceQueue`'s `void` return forces (a `VK_NULL_HANDLE` out of a live call is `XUD_VULKAN_ERROR`, not success); `goto fail` destroys a live device before freeing. `xud_device_wait_idle` is the teardown guard (the plan's `xud_wait_idle`, layer-prefixed per the repo rule); `xud_device_close` is guard → destroy → free, and a **lost** device is still destroyed — skipping `vkDestroyDevice` would leak an object the driver still holds — with `XUD_LOST` still propagated. **What the validation layer caught and the driver did not** (probed with and without `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`: without it, `queueCount+1`, duplicate family rows, unstamped create-infos, `queueCount = 0`, no queue rows at all, priorities `0.0`/`2.0`, the protected flag all answer `VK_SUCCESS` with a live device; with it each is a hard message — `pQueuePriorities is NULL` (`VUID-VkDeviceQueueCreateInfo-pQueuePriorities-parameter`), `queueFamilyIndex (99) is not one of the queue families given via VkDeviceQueueCreateInfo` (`VUID-vkGetDeviceQueue-queueFamilyIndex-00384`), `queueIndex (1) is not less than the 1 queues requested` (`00385`), `Total queue count requested from queue family index 0 is 2, which is greater than queue count available (1)`, `pQueueCreateInfos[1] uses the same queueFamilyIndex (0) and flags as [0]`, `pCreateInfo->sType must be VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO`, `pQueueCreateInfos[0].sType must be VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO`, `queueCount must be greater than 0`) — which is why the row is built from the recorded caps with `queueCount = min(family row, ceiling)`, stamped, and pointed at the struct's **own** `queuePriorities[]` array (the row's pointer must outlive the call). An unadvertised device extension answers `VK_ERROR_EXTENSION_NOT_PRESENT` (`-7`) and an absent feature asked by name answers `VK_ERROR_FEATURE_NOT_PRESENT` (`-8`), both mapped to `XUD_UNSUPPORTED` by `xud_result_from_vk`. **Probed on lavvk here**: `VK_KHR_SWAPCHAIN` is at row 84 of the 196 advertised device extensions, so exactly one name is enabled; `VkDeviceCreateInfo.pNext` stays NULL because the caps' three `*2` chains came back empty (S5.4's bullet); `robustBufferAccess`/`samplerAnisotropy` come out VK_TRUE and `wideLines` VK_FALSE in the create info, and `geometryShader` — which lavvk has and the library does not want — stays VK_FALSE; the one queue family rowed 1 queue, so both role queues are the same handle and `graphics.queueIndex < row.queueCount` holds. **Tests**: `xud_test_device_pure()` headless (synthetic caps + a synthetic live device: the row/extension/create-info assembly rules incl. the extension-storage overflow, the queue lookup's rejection arms, every argument guard of the three driver functions reached without a driver, the unbound-slot state of a fresh loader, a handleless guard answering `XUD_ALREADY`, a handleless close answering `XUD_OK` with no driver call) and `xud_test_device_gpu(loader, physical, instance)` called from **inside** `xud_test_gpu_physical()` before the physical device is closed — a `VkDevice` is created from a `VkPhysicalDevice` handle, so its teardown must happen while its owner is alive, the order `xud_close_context()` uses. Bar met: gcc **and** clang `-fsyntax-only -std=c11 -Wall -Wextra -Wpedantic -Werror` clean over the whole tree (every `.c` in `src/xud`, `tests/xud_test.c`, `examples/hello_context.c`); `ninja -C build` green; headless **465 checks / 0 failures** (was 361), `XUD_GPU_TESTS=1` **615 / 0** (was 463) and validation-clean with `VK_INSTANCE_LAYERS` unset **and** forced to `VK_LAYER_KHRONOS_validation` (the only `ERROR/validation` lines in either run are the diagnostics layer's own synthetic headless probe); `-DXUD_SANITIZERS=ON` (`build-san`) green both ways (465 / 615, no report); `ar -t build/libxud.a` lists **7** objects with `xud_device.c.o`, `nm -u build/xud_test` still lists no `vk*` symbol, `ldd` still lists no `libvulkan`/`libdl`, `hello_context` still links and still answers `XUD_UNSUPPORTED`. **Honest boundary**: the device layer is implemented, in the archive, and tested against the driver, but **nothing in the context calls it yet** — `xud_core.h`'s `device` field is still a `void *` sentinel and `xud_init_context` still answers `XUD_UNSUPPORTED` for valid args; wiring the context to it (and the surface/swapchain below it) is S8's ownership step. The queue-role pair, the 2-queue ceiling and the neutral `1.0f` priority are library decisions today, not policy: nothing orders queues yet, and the row asks for what the family has, never more.

- 2026-10-18 — **the physical layer's driver half, and the layer landing (S5.4, carrying S5.5/S5.6/S5.7/S5.8)**: `src/xud/xud_physical.c` gains the two definitions the header declared and nothing defined — `xud_physical_query_devices(const XudLoader *, VkPhysicalDevice **out, uint32_t *outCount, VkInstance)` (resolve `vkEnumeratePhysicalDevices` through the live instance, nil slot → `XUD_UNSUPPORTED` and never called; the doubling-capacity shape 16→512 because `pCount` is input/output and there is no count-only form, `VK_INCOMPLETE` retried with a doubled capacity, never treated as success-with-a-short-list; a `VK_SUCCESS` that wrote nothing is `XUD_NO_PHYSICAL_DEVICE`, not success) and `xud_physical_open(XudPhysicalDevice **out, XudLoader *, VkInstance)` (**loader is non-const** — `xud_loader_bind` writes slots, and both compilers reject `const XudLoader *` into `XudLoader *` and `&loader->slot` (a `const PFN_vkX *`) into the `void *` out-param, verified with a scratch TU; the query that binds nothing keeps `const`). One allocation owns the struct and its ~1.7 KB caps; six slots are bound through the live instance (`vkEnumeratePhysicalDevices`, the three void `*2` caps getters, `vkEnumerateDeviceExtensionProperties`, `vkGetPhysicalDeviceQueueFamilyProperties2`) and each is checked nil before its first call; the `VkPhysicalDeviceProperties2` array for the selection pass is allocated and stamped per candidate, selection runs on it, and only the chosen row is copied into the caps. **The zero-state discipline the item named, plus what the validation layer did with it**: every `*2` wrapper is stamped (`sType` set, `pNext` NULL) **before** its void getter and checked for a `pNext` write-back **after** it — lavvk answers `VK_SUCCESS` with `pNext` still NULL for all three, so the caps' three `pNext` fields stay NULL and S6's `VkDeviceCreateInfo` chain stays empty; a driver that chains a struct xudlibc does not model is refused at the caps, not silently dropped. The queue-family query stamps **the whole capacity** of rows before the call, not only the rows the driver writes — found because the validation layer answered `vkGetPhysicalDeviceQueueFamilyProperties2KHR(): pQueueFamilyProperties[i].sType must be VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2` for every row up to its duplicate-message limit, and AGENTS.md says validation output is a test signal, so a debug GPU run must be validation-clean, not just crash-free; the device-extension array needs no stamp (`VkExtensionProperties` has no `sType`). `xud_physical_close` is unchanged (free-only, no driver call — there is no `vkDestroyPhysicalDevice`). `xud_core.h`'s `void *physicalDevice` sentinel becomes `XudPhysicalDevice *physicalDevice` (the caps live inside it, so the Pending comment loses that line and keeps device/queue/surface/swapchain/allocator), `xud_init_context` calls `xud_physical_open` between `xud_instance_create` and the `XUD_UNSUPPORTED` return, `xud_close_context` frees it **before** `xud_instance_destroy` — free-only, the comment naming that every other rung of that ladder is a driver call. `CMakeLists.txt`: the stopgap that compiled `xud_physical.c` into the test binary is gone, the TU joins `add_library(xud STATIC ...)` (6 objects, `ar -t build/libxud.a` lists `xud_physical.c.o`), the comment above the target names the physical layer's driver calls as reached only through the table. `tests/xud_test.c` gains `xud_test_gpu_physical(loader, instance)` running **inside** `xud_test_gpu_instance()` before the instance is destroyed (the physical device is owned by the instance — same ownership rule as the diagnostics messenger's teardown inside `xud_instance_destroy()`), and `xud_test_physical_negative()` headless (nil table → `XUD_UNSUPPORTED`, NULL out-params → `XUD_INVALID_ARG`, a NULL-in-handling close → `XUD_ALREADY`, the caps' `pNext` rule). **Probed numbers, written into AGENTS.md**: 1 physical device, `apiVersion = 4211042` (= `VK_MAKE_API_VERSION(0,1,4,363)`), `deviceType = INTEGRATED_GPU`, vendor 0x8086 / device 0x400, name "Intel Iris Graphics … (TigerLake-LP GT2 …)"; **196** device-level extensions through a NULL `pLayerName`, `VK_KHR_SWAPCHAIN` among them; **1** queue family, `queueFlags = 0x1f` (GRAPHICS|COMPUTE|TRANSFER|SPARSE_BINDING|PROTECTED), `queueCount = 1`, `maxImageSampleCount = 1`, `timestampValidBits = 36`; limits read into the caps: `maxPushConstantsSize` **128** (the guaranteed floor — no headroom here for the design sketch's push-constant struct), `maxCombinedImageSamplerDescriptorCount` **1** (so a set-1 sampler array of more than one is gated on caps, not assumed), `maxUniformBufferPerStageSize` 65536, `maxStorageBufferRange` 4294967295, `maxBufferSize` 4294934527, `maxVertexInputAttributes` 32, `maxDescriptorSetUniformBuffers` 65536, `maxComputeWorkgroupSize` 1024, `maxImageDimension2D` 16384; memory 3 heap types / 3 heaps, `VK_MEMORY_HEAP_DEVICE_LOCAL` 4294967295 B, `VK_MEMORY_TYPE_UNIFORM_BUFFER` size 16384 align 256. Wanted features on this driver: `robustBufferAccess` **present**, `samplerAnisotropy` **present**, `wideLines` **absent** — so the ∩ rule leaves `wideLines` off and the GPU test asserts exactly that (lines fall back to width 1 rather than failing). Bar met: gcc **and** clang `-fsyntax-only -std=c11 -Wall -Wextra -Wpedantic -Werror` clean over the whole tree; `ninja -C build` green; headless **361 checks / 0 failures** (was 339), `XUD_GPU_TESTS=1` **463 / 0** (was 401) and **validation-clean** (the only `ERROR/validation` line in either run is the diagnostics layer's own synthetic headless probe, the counters stay at 0 from the driver); `-DXUD_SANITIZERS=ON` (`build-san`) green both ways, 361 / 463, no sanitizer report; `ar -t build/libxud.a` lists **6** objects with `xud_physical.c.o`; `ldd build/xud_test` still lists no `libvulkan`/`libdl`, `nm -u build/xud_test` lists no `vk*` symbol, `hello_context` still links and still answers `XUD_UNSUPPORTED`. Vulkan-Headers moved **1.4.357 → 1.4.363** on this machine during the step (`VK_HEADER_VERSION 363`, `libvulkan.so.1 → libvulkan.so.1.4.363`, `vulkan-validation-layers 1.4.363.0-1`); every line number grepped for the physical layer (5809/5803/5843/5837/5827/5821/4544/6182) is identical in both versions, so the header-facts block stays true, and AGENTS.md's toolchain table is corrected to 1.4.363. Honest boundary: `xud_init_context` still returns `XUD_UNSUPPORTED` for valid args — loader, diagnostics, instance and physical are opened and torn down, nothing below the physical layer exists (no `VkDevice`, no queue, no surface, no swapchain, no allocator); the `XUD_OUT_OF_RESOURCES` overflow of `xud_physical_select_queue_family` is exercised only against the synthetic row (a driver answering more families than the cap is not reachable here), and the device-extension list's growth failure is `XUD_OUT_OF_MEMORY`, which the headless run cannot reach without a driver; the discrete-GPU preference policy stays a decision (the architecture-id chained struct is not in these headers), and the present half of AGENTS.md's one-family rule stays GLFW's check at S7 (there is no present bit — see the S5.2 grep).

- 2026-10-18 — **the physical layer's pure half (S5.3)**: `src/xud/xud_physical.c` lands with the five definitions that need no driver call — `xud_physical_select` ("first in driver order", ranking stays a decision because the architecture-id chained struct is not in these headers), `xud_physical_record_features` (a file-scope `static const VkPhysicalDeviceFeatures XUD_PHYSICAL_WANTED_FEATURES` ∩ the driver's `caps->features.features` through one helper that normalises the output to VK_TRUE/VK_FALSE, so the struct S6 hands to `VkDeviceCreateInfo` is exactly 0/1 and an unverified feature is never forced on; wanted today = `robustBufferAccess` + `samplerAnisotropy` + `wideLines`, each with its reason, and the driver's verbatim answer stays in `caps->features`), `xud_physical_select_queue_family` (first family whose `queueFlags` carry `VK_QUEUE_GRAPHICS_BIT`, the whole `VkQueueFamilyProperties2` row copied once, `XUD_PHYSICAL_NO_QUEUE_FAMILY` + a **zeroed** row on refusal so S6 cannot read a rejected family as a queue to create, transfer/compute/granularity recorded but never required), `xud_physical_record_api_version` (the driver's answer read into `caps->apiVersion` verbatim even when the floor check fails, `XUD_UNSUPPORTED` below `XUD_INSTANCE_API_FLOOR`), and `xud_physical_close` (free-only — the caps' one heap member then the struct, counts cleared, `XUD_ALREADY` on a NULL-in-handling pointer, no driver call, which is why it lands here: the sanitizer run of that teardown is this step's bar). The two driver definitions (`xud_physical_query_devices`, `xud_physical_open`) are declared by the header and defined nowhere — legal today because this TU is not in the archive yet and nothing calls them. Six `static_assert`s with messages (the graphics bit is non-zero, exactly one bit, distinct from the transfer bit; `VK_FALSE == 0u`/`VK_TRUE == 1u`; the API-version encoding is ordered oldest-first) and **one assert removed**: `sizeof(WANTED) == sizeof(VkPhysicalDeviceFeatures)` looked like the item's guarantee but both spellings are the same typedef, so it was a tautology — the real compile-time check is the designated initializers, which fail if the wanted struct is ever spelled as the `...2` wrapper. The member-path trap this step found is now written into the file-top comment: from the caps the apiVersion is **two** hops (`caps->properties.properties.apiVersion`, the caps field is itself named `properties`), from a bare `VkPhysicalDeviceProperties2` it is **one** (`props[i].properties.deviceType`) — the test fixture wrote the two-hop form against a one-hop variable and gcc answered `'VkPhysicalDeviceProperties' has no member named 'properties'`. `tests/xud_test.c` gains `xud_test_physical_pure` (headless, synthetic-everything: null/empty guards, selection over a three-device list whose `deviceType` differs and must not reorder, the ∩ rule in all four shapes plus a non-normalised driver answer, the queue rule incl. a compute-only family that must not be chosen and a graphics-only family that must be, the sentinel + zeroed row on refusal, `apiVersion` at exactly the floor and one below it, `xud_physical_close` on a heap caps struct with and without the extension array) and `CMakeLists.txt` compiles `src/xud/xud_physical.c` **into the `xud_test` executable**, not the archive — the pure half exists, the driver half does not, so the only way to test it at all is to build this TU into the test binary; S5.6 adds the archive line when the contract is complete. Bar met: gcc **and** clang `-fsyntax-only -std=c11 -Wall -Wextra -Wpedantic -Werror` clean over the whole tree, `ninja -C build` green, headless **339/0** (was 281) and `XUD_GPU_TESTS=1` **401/0** (was 343), `-DXUD_SANITIZERS=ON` green both ways, archive unchanged at 5 objects (`ar -t build/libxud.a` does not list `xud_physical.c.o`), `ldd` still lists no `libvulkan`/`libdl` and `nm -u` no `vk*`. Honest boundary: `xud_init_context` still returns `XUD_UNSUPPORTED` for valid args — the physical layer's pure functions are testable but nothing in the library calls them yet, no `VkPhysicalDevice` is enumerated, and `xud_physical.c` is not in the archive; `xud_test_physical_pure` covers the pure half only, `xud_test_gpu_physical` and the caps-storage overflow check stay in S5.7 because both need S5.4's driver half.

- 2026-10-18 — **the physical layer's header, declarations only (S5.2)**: `src/xud/xud_physical.h` lands as the layer's own header, name `xud_physical` and not `xud_device` — S6 owns `VkDevice` and `xud_device.h/.c` is what that layer will want, so the two would collide; the rule is written into the header's file-top comment, not left to the implementer. `XudPhysicalDevice` = the `VkPhysicalDevice` handle + an embedded-by-value `XudDeviceCaps` (~1.7 KB with `VkPhysicalDeviceProperties2` 840 B, `VkPhysicalDeviceFeatures2` 240 B, `VkPhysicalDeviceMemoryProperties2` 536 B and the chosen `VkQueueFamilyProperties2` 40 B row inside it — measured here, which is why the struct is heap-allocated by `xud_physical_open` and never a stack temporary); the caps are the name AGENTS.md's Vulkan rules already fix — the three `*2` structs copied in, the advertised **device**-extension list as a heap array sized on the doubling sequence (the only heap member, freed by `xud_physical_close`), the chosen queue-family index + its row, `apiVersion` read back. Macros `XUD_PHYSICAL_QUERY_MIN_CAPACITY` 16u / `XUD_PHYSICAL_QUERY_MAX_CAPACITY` 512u (the doubling sequence the two input/output-count queries here need — `vkEnumeratePhysicalDevices` and `vkEnumerateDeviceExtensionProperties` have no count-only form, `pCount` is input/output, same shape as `xud_instance_query_extensions` and `xud_diagnostics_query_layers`) and `XUD_PHYSICAL_NO_QUEUE_FAMILY` 0xFFFFFFFFu; five `static_assert`s, each with a message (clang 22 rejects the message-less form here): the cap sequence is sane and doubling cannot overflow `uint32_t`, the sentinel is outside every index the queue-family query can return, `sizeof(VkExtensionProperties) == VK_MAX_EXTENSION_NAME_SIZE + sizeof(uint32_t)` (256 + 4, no padding — the caps' heap extension array is one plain block, checked not trusted), and `XUD_INSTANCE_API_FLOOR >= VK_API_VERSION_1_0` (one macro owns that number, seen through the include — no second macro that could drift). The naming trap the header exists to record: the queue-family wrapper is `VkQueueFamilyProperties2` (:5837) — no `PhysicalDevice` prefix, unlike the other three — whose core struct is `VkQueueFamilyProperties` (:5827), while `VkPhysicalDeviceQueueFamilyProperties` (:5821) is a *different* thing, a `VkPhysicalDeviceProperties2` `pNext` chain entry; three names within 20 lines of each other. **Two greps that change the plan's wording, done today in `/usr/include/vulkan/` (no driver involved)**: `VK_QUEUE_PRESENT` → **zero hits** across the whole tree (the queue flags are GRAPHICS 0x1, COMPUTE 0x2, TRANSFER 0x4, SPARSE_BINDING 0x8, PROTECTED 0x10 + vendor bits), and presentation is a per-window-system query (`vkGetPhysicalDeviceWaylandPresentationSupportKHR`/Xcb/Xlib/Win32/DirectFB/UBM/Screen, no core form) = GLFW's `glfwGetPhysicalDevicePresentationSupport` at S7 — so the queue-family rule this layer can enforce is the **graphics** bit, the present half of AGENTS.md's one-family rule is GLFW's check at S7, and S5.3/S5.7's "both graphics and present" wording is corrected in `todo.md` so nobody chases a bit that is not there; second, `VkPhysicalDeviceArchitectureInfo` → **zero hits**, so the only core fields a discrete-preference policy could rank on are `deviceType` (the enum at :2233–:2238) + `vendorID`/`deviceName`/`pipelineCacheUUID` — enough to prefer a discrete GPU, not enough to identify one — which is why `xud_physical_select` ranks on nothing today and "policy later" stays a decision, not code. Also recorded: the four caps getters return **void** (`PFN_vkGetPhysicalDeviceFeatures2` :6178, `PFN_vkGetPhysicalDeviceProperties2` :6179, `PFN_vkGetPhysicalDeviceMemoryProperties2` :6183, `PFN_vkGetPhysicalDeviceQueueFamilyProperties2` :6182) — no `VkResult` for `xud_result_from_vk` to map, so the caps are zeroed as part of the one allocation and "the struct still says nothing" is the only failure signal those four give (apiVersion 0 then fails the floor check); the two that do answer with a `VkResult` are `vkEnumeratePhysicalDevices` :4534 and `vkEnumerateDeviceExtensionProperties` :4544. No new loader slot: `XUD_LOADER_SLOT_COUNT` stays **19**, S5.1 already slotted all six physical names, this header declares no PFN and binds nothing. Bar met: gcc **and** clang `-fsyntax-only -std=c11 -Wall -Wextra -Wpedantic -Werror` clean over the whole tree **and** on a TU that includes only `xud_physical.h` (the header must be includable on its own — it pulls `xud.h`, `xud_loader.h`, `xud_instance.h`, `xud_vulkan.h`, `<vulkan/vulkan.h>` itself) in all three flag sets the build uses (`-g -DXUD_DEBUG=1`, `-O2 -DXUD_DEBUG=0 -DNDEBUG`, `-fsanitize=address,undefined -g`); `ninja -C build` and `ninja -C build-san` unchanged (the header is not in the archive yet — S5.6 adds the `.c` line), `./build/xud_test` green headless (**281/0**) and with `XUD_GPU_TESTS=1` (**343/0**) — same numbers as S4/S5.1, this step adds no call site and no test; `-DXUD_SANITIZERS=ON` green; link still Vulkan-free (`ldd build/xud_test` lists no `libvulkan`/`libdl`, `nm -u` lists no `vk*`). AGENTS.md gains a *Verified physical-layer header facts* block and the *Present today* row names the new header as header-only. Honest boundary: the header declares six functions and defines none — `xud_physical.c` is S5.3's (pure) and S5.4's (driver), and the archive is unchanged, so nothing in the tree references them yet and no test includes the header yet (S5.7's job); `xud_init_context` still returns `XUD_UNSUPPORTED` for valid args, nothing below the instance exists.

- 2026-10-18 — **loader table grows by the physical layer's `*2` slot (S5.1)**: `src/xud/xud_loader.h` gains `PFN_vkGetPhysicalDeviceProperties2 getPhysicalDeviceProperties2` in the instance-level tier, directly after the non-2 form it pairs with — the only name the physical layer needs that the table did not already have (`vkEnumeratePhysicalDevices`, `getPhysicalDeviceProperties`, `getPhysicalDeviceFeatures2`, `getPhysicalDeviceQueueFamilyProperties2`, `enumerateDeviceExtensionProperties` came in with the instance layer) — `XUD_LOADER_SLOT_COUNT` 18 → **19** with the "grep -c, keep exact" comment still honest, and the *Not slotted* comment now records why the *2 physical queries **are** slotted while the messenger pair is not (probed here today, dlopen only: all six physical names resolve through a **bare** instance — `apiVersion = VK_API_VERSION_1_0`, no extension, no layer — and through the instance `xud_instance.c` creates, none of them with `VK_NULL_HANDLE` in hand, so they are instance-level; and they name the driver's own code, not a per-instance layer, so a cached slot survives an instance recreation). lavvk answers the *2 forms even with `VK_KHR_get_physical_device_properties2` not enabled, so the slot fills either way here; a driver that gates them answers NULL and the binding layer reports the nil slot rather than calling through it. `XUD_LOADER_GLOBAL_SLOT_COUNT` stays 3, `xud_loader.c` untouched, no test or example change (S5.7's job). Bar met: gcc **and** clang `-fsyntax-only -std=c11 -Wall -Wextra -Wpedantic -Werror` clean over the whole tree (all `src/xud/*.c`, `tests/xud_test.c`, `examples/hello_context.c`, and a TU including only `xud_loader.h`) in all three flag sets the build uses; `ninja -C build` green; `./build/xud_test` **281 checks / 0 failures** headless and **343 / 0** with `XUD_GPU_TESTS=1` (same numbers as S4 — this step adds no call site and no test); `-DXUD_SANITIZERS=ON` green; `ldd` still lists no `libvulkan`/`libdl` and `nm -u` no `vk*`. Honest boundary: the slot is filled by nothing yet — S5.4 resolves and stores it through a live `VkInstance`, so today the table simply has one more instance-level slot than the layers above use; `xud_init_context` still returns `XUD_UNSUPPORTED` for valid args and nothing below the instance exists.
- 2026-10-16 — **diagnostics layer (S4)**: `src/xud/xud_diagnostics.h`/`.c` (the layer between the loader and the instance: `XudDiagnostics` = advertised layers in fixed storage + a `const char *` pointer array into those rows (because `ppEnabledLayerNames` is `const char *const *` and a `char (*)[256]` is not assignment-compatible with it), the `VkValidationFeaturesEXT` request, the messenger create-info, the messenger handle, and the `messagesReceived`/`errorMessages` counters; pure parts `xud_diagnostics_wanted_extensions` (the one mechanism for the diagnostics wanted list — `VK_EXT_debug_utils`, `VK_EXT_validation_features` — accessor, not an extern array), `xud_diagnostics_fill_validation_features` (best-practices + synchronization only, nothing disabled), `xud_diagnostics_fill_messenger_create_info` (all severities, all types, `pUserData` → the struct), `xud_diagnostics_select_layers` (advertised order, duplicate names skipped, `XUD_OUT_OF_RESOURCES` at the cap), `xud_diagnostics_severity_name`, `xud_diagnostics_message_callback` (counts into the struct it was handed as `pUserData`, counts nothing when it is NULL — there is no struct to count into — survives a NULL `callbackData`/`pMessage`, always returns `VK_FALSE`); driver parts `xud_diagnostics_query_layers`/`open` (global-level `vkEnumerateInstanceLayerProperties`, same input/output `pCount` shape as the extension query, doubling capacity 16→512) and `xud_diagnostics_create_messenger`/`close` (the two messenger entry points resolved **on demand through a live `VkInstance`**, never cached in a PFN slot — `vkGetInstanceProcAddr` answers NULL for them unless `VK_EXT_debug_utils` is enabled on the instance in hand, and a resolve cached across an instance recreation is a stale pointer into a dead layer); `xud_instance.h`/`.c` catch up with the diagnostics struct (`xud_instance_select_extensions` takes the diagnostics wanted list through the accessor, `xud_instance_fill_create_info` takes `const XudDiagnostics *` and puts `VkValidationFeaturesEXT` on the `pNext` chain **only when every extension the diagnostics layer asked for was actually enabled** — `xud_instance_all_names_enabled`, because a `pNext` chain may carry a struct for an enabled extension and nothing else — and `xud_instance_destroy` tears the messenger down first, through the `VkInstance` that is about to go away, then clears the borrowed handles without freeing the struct); `xud_core.h`/`.c` gain the `diagnostics` field (`NULL` in a release build = no layer requested, no validation request, no messenger), `xud_init_context` opens it between loader-open and instance-create behind `context->validate != 0u` and creates the messenger after the instance (`XUD_UNSUPPORTED` from that call is the honest "no log side on this driver", every other failure goes to the `goto fail` ladder), `xud_close_context` runs `xud_instance_destroy` → `xud_diagnostics_close` → `xud_loader_close`; `xud_loader.h` records why the messenger entry points are **not** slots of the PFN table (`XUD_LOADER_SLOT_COUNT` stays 18), `CMakeLists.txt` adds `xud_diagnostics.c` (still no `-lvulkan`, no `-ldl`), `xud.h`/`hello_context.c` file-tops name the layer, `AGENTS.md` gains a *Verified diagnostics bring-up* block and the *Present today* row names the pair. Bar met: `gcc`/`clang -fsyntax-only -std=c11 -Wall -Wextra -Wpedantic -Werror` clean over the whole tree, `./build/xud_test` **281 checks / 0 failures** headless, `XUD_GPU_TESTS=1` **343 / 0** with `VK_INSTANCE_LAYERS` unset and forced to `VK_LAYER_KHRONOS_validation` (lavvk: 3 layers advertised — `VK_LAYER_MESA_device_select`, `VK_LAYER_MESA_anti_lag`, `VK_LAYER_KHRONOS_validation` — 26 instance-level extensions, 4 enabled with the diagnostics struct = the three wanted ones + `VK_EXT_debug_utils`, `VK_EXT_validation_features` not advertised so the chain stays empty, messenger live, **0 messages received = validation-clean**), `-DXUD_SANITIZERS=ON` green, the `-DXUD_DEBUG=OFF` config (diagnostics `NULL`, no layer requested) syntax-clean and its `xud_test` green too, `hello_context` still answers `XUD_UNSUPPORTED` and is leak-free in debug and release configs. Link still Vulkan-free: `ldd build/xud_test` lists no `libvulkan`/`libdl`, `nm -u` no `vk*`. Honest boundary: `xud_init_context` still returns `XUD_UNSUPPORTED` for valid args — loader + diagnostics + instance are opened, created and torn down, nothing below the instance exists; `VkDebugUtilsMessengerCreateInfoEXT` is deliberately **not** on the create-info `pNext` chain (probed twice: this loader segfaults inside `vkCreateInstance` with a messenger there, while the standalone create/destroy pair works cleanly and `VkValidationFeaturesEXT` on the chain is accepted).
- 2026-10-16 — **instance layer (S3)**: `src/xud/xud_vulkan.h`/`.c` (the shared vocabulary of the Vulkan-calling layers: `xud_result_from_vk` — the one funnel every `VkResult` goes through, `VK_INCOMPLETE` deliberately mapped to `XUD_VULKAN_ERROR` and not `XUD_OK`, since in Vulkan it is not an error but "the list was cut" — plus `xud_name_equal` (case-sensitive, no lexicographic order claimed) and `xud_name_copy`, a fixed-buffer name copy that rejects a name that does not fit rather than truncating it; pure, no driver call, so the headless run covers them completely), `src/xud/xud_instance.h`/`.c` (`XudInstance` = the handle plus the `VkApplicationInfo` that produced it plus the selected extension names in fixed storage, so the create-info struct is reproducible from the struct alone; pure parts `xud_instance_fill_application_info` (names = `XUD_INSTANCE_NAME`, version = `VK_MAKE_API_VERSION` of the xud.h macros, `apiVersion = XUD_INSTANCE_API_FLOOR` = `VK_API_VERSION_1_0` — the floor, not the header's newest), `xud_instance_select_extensions` (wanted ∩ advertised, in wanted order, selection rewritten every call so a re-query never accumulates, advertised NULL is "nothing advertised" → enable nothing), `xud_instance_fill_create_info` (points `ppEnabledExtensionNames` at a caller-owned array of `XUD_MAX_INSTANCE_EXTENSIONS` slots whose tail is NULLed — the array is not part of the struct because the struct must stay copyable); driver parts `xud_instance_create` (query the advertised list with a doubling capacity — `pCount` is input/output, there is no count-only form, verified here that a zero-capacity call writes 0 and returns `VK_INCOMPLETE`; a driver that reports `VK_SUCCESS` while writing more than the capacity is treated as the same cut-list shape and retried, never as success with a short list; bind `vkCreateInstance` through the seed; `goto fail` destroys whatever was made) and `xud_instance_destroy` (resolves `vkDestroyInstance` **through the live instance** and caches it in the table — `vkGetInstanceProcAddr` answers NULL for instance-level commands without a handle, verified here; `XUD_ALREADY` on a second destroy, never a double free; a handle-less instance is freed with no driver call), `xud_loader.h`/`.c` gains `xud_loader_bind` (public: resolve + move the address into a slot by `memcpy`, `resolvedCount` bumps only on empty→filled so it is a property of the table) and `xud_loader_is_open` (the invariant every layer above needs), and `xud_loader_resolve`'s handle widens `VkInstance` → `const void *` so a `VkPhysicalDevice`/`VkDevice` is not an incompatible-pointer argument (the one cast back to `VkInstance` is guarded by `static_assert(VK_USE_64_BIT_PTR_DEFINES == 1, ...)`), `xud_core.h`/`.c` gains the `instance` field, `xud_init_context` opens the loader then creates the instance and still reports `XUD_UNSUPPORTED` (device/queue/surface/swapchain pending), `xud_close_context` destroys the instance before closing the loader, `CMakeLists.txt` adds both new `.c` (still no `-lvulkan`), `tests/xud_test.c` gains `xud_test_result_mapping`/`xud_test_name_helpers`/`xud_test_instance_pure` (headless, complete coverage of the pure parts incl. wanted-order selection and the NULLed tail) and the first GPU-gated test `xud_test_gpu_instance` (create/destroy twice on one table, `XUD_ALREADY` on the third, handle-less teardown). Bar met: `gcc`/`clang -fsyntax-only` clean, `./build/xud_test` **180 checks / 0 failures** headless, `XUD_GPU_TESTS=1` **205 / 0** (lavvk: 26 instance-level extensions advertised, all three wanted ones enabled in wanted order, `vkCreateInstance` at the 1.0 floor → `VK_SUCCESS`), `-DXUD_SANITIZERS=ON` green both ways (1000 init+close = 1000 dlopen/dlsym + create/destroy cycles, leak-free), and the **no-driver path** re-verified by an `LD_PRELOAD` shim interposing `dlopen` to hide `libvulkan.so.1` → 162 checks / 0 failures headless and under ASan, `xud_vulkan_supported() -> XUD_UNSUPPORTED`, rc 0. Link still Vulkan-free: `ldd build/xud_test` lists no `libvulkan`/`libdl`, `nm -u` no `vk*`. Honest boundary: `xud_init_context` still returns `XUD_UNSUPPORTED` for valid args — loader + instance are opened, created and torn down, nothing below the instance exists; validation layers are still not requested (`enabledLayerCount == 0`, the diagnostics step's job), so the GPU run is not yet validation-layer-clean.
- 2026-10-16 — **S2 loader bring-up**: `src/xud/xud_loader.h` (the loader layer's own header: `XudLoader` = the `dlopen` handle + a **typed** PFN table of 18 slots in four tiers — 3 global-level (`getInstanceProcAddr`, `enumerate*Instance*`), 5 instance-level, 4 device-level, 6 swapchain/surface-teardown — each tier labelled with the bring-up step that fills it, `XUD_LOADER_GLOBAL_SLOT_COUNT`/`XUD_LOADER_SLOT_COUNT`, `XUD_VULKAN_LIBRARY_SONAME "libvulkan.so.1"`, `XudPfnBits` union for the cast-free copy, `xud_loader_open`/`xud_loader_close`/`xud_loader_resolve`), `src/xud/xud_loader.c` (`dlopen(RTLD_NOW|RTLD_GLOBAL)` → `dlsym("vkGetInstanceProcAddr")` seed → seed resolves the global-level entry points → `resolvedCount` must equal the global count or the open fails; `xud_pfn_copy` = `memcpy` into the PFN slot; every nil PFN reported `XUD_UNSUPPORTED`, never called; `goto fail` ladder closes a half-open loader), `xud.h` gains `xud_vulkan_supported()` (a real runtime query — opens/closes a loader, no cache), `xud_core.c` opens the loader into `XudContext` and still returns `XUD_UNSUPPORTED` (instance/device/surface pending), `xud_close_context` closes the loader, `CMakeLists.txt` adds `xud_loader.c` (still no `-lvulkan`, no `-ldl`), `tests/xud_test.c` gains loader checks that **degrade to a note when `dlopen` fails** so the headless run stays green with no driver. Bar met: `gcc`/`clang -fsyntax-only` clean, `./build/xud_test` **105 checks / 0 failures**, `XUD_GPU_TESTS=1` run green (still no GPU check — S3 is the first), `-DXUD_SANITIZERS=ON` build green (ASan+UBSan, 1000 init+close = 1000 dlopen/dlsym/dlclose cycles leak-free), and the **no-driver path verified** by an `LD_PRELOAD` shim interposing `dlopen` to hide `libvulkan.so.1` → 88 checks / 0 failures, `xud_vulkan_supported() -> XUD_UNSUPPORTED`, rc 0. Link verified clean: `ldd build/xud_test` shows no `libvulkan`/`libdl`, `nm -u` shows no `vk*` symbol. Loader facts recorded in `AGENTS.md` (PFN typedefs present in `vulkan_core.h`, the strict-mode `dlsym`→PFN cast rule, the `static_assert` message rule, `VK_NULL_HANDLE` is `0U`). Honest boundary: `xud_init_context` still returns `XUD_UNSUPPORTED` for valid args — the loader is opened and closed, nothing is created.
- 2026-10-16 — **S1 repo skeleton**: `src/xud/xud.h` (version macros `XUD_VERSION_*`/`XUD_VERSION`, config macros `XUD_VALIDATE` (debug-gated, runtime override `VK_INSTANCE_LAYERS`) and `XUD_GPU_TESTS` (env override of the same name), `XudResult` 9-member error enum, `xud_result_name()`, opaque `XudContext`, `xud_init_context`/`xud_close_context`), `src/xud/xud_core.h` (the struct that completes the opaque type, every Vulkan field a sentinel with the step that fills it named beside it), `src/xud/xud_core.c` (arg validation, shell `calloc`, `goto fail` teardown ladder, `xud_close_context` frees + sets NULL), `tests/xud_test.c` (74 checks, headless, deterministic, GPU gate present but empty), `examples/hello_context.c`, `CMakeLists.txt` (static lib `xud` + `xud_test` + `hello_context`, no `-lvulkan`). Bar met: `gcc -fsyntax-only` and `clang -fsyntax-only` clean, `./build/xud_test` green (74/0), `XUD_GPU_TESTS=1` run green, `-DXUD_SANITIZERS=ON` build green (ASan+UBSan, 1000 init+close iterations leak-free). Honest stub boundary: `xud_init_context` returns `XUD_UNSUPPORTED` for valid args and leaks nothing — nothing below the context shell exists until S2. Two **toolchain corrections** found and applied to `AGENTS.md`: `cmake -B build -G ninja` is rejected by cmake 4.4.3 (generator name is `Ninja`), and `-Ddefault=lib` is a macro define of `default` under gcc/clang here, which rewrites every `switch` label and breaks the build — it is not used anywhere.
- 2026-10-16 — Commit policy sharpened into a four-point **Commit rule** in `AGENTS.md` (todo item ticked/moved + syntax check and tests green + nothing papered over + docs match the tree ⇒ commit immediately, one commit per step, never a state you are unsure about). First commit made: `.gitignore`, `AGENTS.md`, `todo.md` — `reference/raylib/` stays untracked and gitignored.
- 2026-10-16 — `reference/raylib/` populated: raylib **6.2** headers copied from `~/repos/xudlib/reference/raylib/src/` (git `6.2-5-gc757c743`) — `raylib.h`, `raymath.h`, `rcamera.h`, `rgestures.h`, `rlgl.h`, `config.h` + `LICENSE` + our own `PROVENANCE.md` (560 KB). `.gitignore` added (`bin/`, `build/`, `reference/`), so `git status` stays clean; `AGENTS.md` gained a *Reference material* section naming every file. Docs/data-only — no library code, no build files.
- 2026-10-16 — Repo created (empty, branch `main`, no commits), `AGENTS.md` + `todo.md` written, design sketched (layer stack, frame loop, pipeline set, descriptor layout, vertex format), work broken into S1–S9. Docs-only — no code, no build files yet.
- 2026-10-16 — Toolchain facts verified on this machine and recorded in `AGENTS.md`: gcc 16.2.1 / clang 22.1.8, cmake+ninja+meson, `vulkan-headers` 1.4.357 (`VK_HEADER_VERSION 357`, newest core macro `VK_API_VERSION_1_4`), `libvulkan.so.1.4.357`, `VK_LAYER_KHRONOS_validation` present, ICDs intel/hasvk/nouveau/radeon, GPU = Intel TigerLake-LP GT2 (Iris Xe), `glslangValidator` 11:16.4.0 + `glslc` 2026.3 + `spirv-val`.
- 2026-10-16 — GLFW facts verified against the installed header (`/usr/include/GLFW/glfw3.h`, `pkg-config glfw3` → 3.5.1): the Vulkan surface API **is** present here (`glfwCreateWindowSurface`, `glfwGetPhysicalDevicePresentationSupport`, `glfwGetRequiredInstanceExtensions`) — unlike the Odin sibling, whose vendored bindings lacked surface creation. `#include <GLFW/glfw3.h>` and `#include <vulkan/vulkan.h>` both compile with `gcc -std=c11 -Wall -Wextra -Wpedantic`.
- 2026-10-16 — Header gap confirmed: `VK_VALIDATE_*` / `VkPhysicalDeviceValidationFeatures` are absent from Vulkan-Headers (grep over `/usr/include/vulkan/vulkan_core.h` finds zero hits for `VK_VALIDATION_FEATURES`); only `VkValidationFeaturesEXT` is available. Same finding as the Odin sibling repo.
- 2026-10-16 — Dependency availability checked: **no** Volk, VMA, miniaudio, cgltf, stb, or `cglm` packaged here, and no Vulkan SDK — so every third-party dep is a vendoring decision, and `AGENTS.md` says asking is required before adding one.
