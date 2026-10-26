# xudlibc — TODO

Progress tracker. Move items between sections as work happens and add a dated one-liner.
Finished items and their long notes move to `todo_completed.md` (newest first) — never delete a finished
step, just move it, so the active tracker stays short and the log stays complete.

## Now

- [ ] Batch layer (`xudrl`): dynamic per-attribute **stream** buffers + one index buffer (SoA, decided 2026-10-23 — see
    Decided below), flush on pipeline/material change (flush is per stream), fixed pipeline set
    - 2026-10-25 note: **nothing of this rung is in the tree** — no `xud_batch.h`/`xud_batch.c`, no batch field in
      `XudContext`, no `tests/xud_test_batch.c`. What is left, itemised against what the tree says today:
      - **The rung**: N stream `VkBuffer`s (`pos`/`uv`/`color`/`normal` = 36 B/vertex across four buffers) + one
        index buffer, each carved out of the allocator — the allocator's **third** thing ever asked for, after the
        descriptor's uniform buffer and the render pass's depth image.
      - **The pure half** (headless-testable by construction): the "every stream holds the same vertex count"
        invariant, the flush-on-pipeline/material-change decision (flush is per stream), the vertex/index
        arithmetic, the capacity-doubling bounds.
      - **The driver half**: staging the CPU-side attribute bytes into those buffers by `vkCmdCopyBuffer` off the
        allocator's transient ring — `xud_allocator_ring_acquire`/`xud_allocator_ring_reset` are called by nothing
        above the allocator today, and there is no `vkCmdCopyBuffer` anywhere in `src/xud/`.
      - **The first three command-buffer bindings the tree would make**: `vkCmdBindVertexBuffers` (one call, whole
        array, returns void → zero-state-then-check), `vkCmdBindDescriptorSets` for set 0, `vkCmdPushConstants`
        into the range the descriptor rung already wrote — none of the three names appears anywhere today.
      - **The per-frame uniform update**: `xud_descriptor_fill_uniform_block()` writes an identity `viewProj`
        (`xud_descriptor.c:387-390`) and nothing else ever touches that row; the copy that puts the block's bytes
        into the GPU buffer is the frame loop's, and is not in the tree.
      - **The pipeline's vertex input**: `xud_pipeline_fill_state()` still writes
        `vertexBindingDescriptionCount`/`vertexAttributeDescriptionCount` 0 (`xud_pipeline.c:80`/`82`) — the batch
        writes four binding + four attribute rows, gated on the caps' `maxVertexInputBindings`/
        `maxVertexInputAttributes`, and the built-in SPIR-V must be rewritten to read those bindings and take the
        push constant (today's shaders generate their three vertices from `gl_VertexIndex`).
      - **The draw**: `xud_frame.c:665` is a hard-coded `vkCmdDraw(commandBuffer, XUD_FRAME_DRAW_VERTEX_COUNT, 1u,
        0u, 0u)`; the batch's is `vkCmdDrawIndexed` against a real index buffer and count.
      - **The second pipeline variant**: the transparent one (blend on + depth test, sorted) — a second pipeline of
        the pipeline rung, created once at init, plus the sorted-batch rule. Only the opaque variant exists today
        (`xud_pipeline.c:178`, `blendEnable = VK_FALSE`).
      - **The handle layer's `record`**: `XudHandleSlot.record` (`xud_handle.h:107`) is still the NULL sentinel and
        `xud.h` still declares no create/close function that takes a handle — the batch/object rungs are what fill
        it.
      - **The frame loop's shape**: `xud_frame.h:50`'s one-`VkSubmitInfo`-per-submit "one batch flushed per frame"
        assumption and `xud_frame_draw()`'s refusal shapes must grow to multi-draw recording.
      - **The test area**: `tests/xud_test_batch.c` + one line in `tests/xud_test_main.c` and one in
        `CMakeLists.txt`'s explicit source list (no globs), per the split's rules.
    - 2026-10-25 note (the reasoning that note above assumes, kept for it): sits above the descriptor rung (S13) and
      the depth-attachment step (S15), both landed the same day — the batch's streams are the allocator's **third
      thing ever asked for (the descriptor's uniform buffer and the render pass's depth image were the first two,
      and the image is a block of its own because an image's `VkMemoryRequirements` ask for an alignment of 65536
      on this driver, not a buffer's 256), and its draws are the
      first thing that binds set 0 and pushes the range that rung already wrote. `vkCmdBindVertexBuffers` and
      `vkCmdBindDescriptorSets` are rows of Volk's device table, already filled in bulk, so no loader slot is added —
      the row check before the call is what remains. This is also the first time the tree writes
      `VkPipelineVertexInputStateCreateInfo` at all: today's pipeline writes zero bindings and zero attributes. **The
      transparent pipeline variant lands here, not earlier**: the opaque variant (blend off + depth write against the
      depth attachment) landed with S15, and the transparent half (blend on + depth test, sorted) is the sorted-batch
      rule's, so it is the batch's to write.

## Next

- [ ] Shapes module (`DrawCube`, `DrawSphere`, `DrawTriangle3D`, `DrawLine3D`, `DrawPoint3D` — 3D only, there is no 2D
    shape family in the API)

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

- Model format: vendor cgltf (glTF 2.0, what raylib does) vs minimal `.glb` reader (ask)
- Audio: vendor miniaudio vs no audio (ask)
- Text/font: own bitmap atlas vs vendored `stb_truetype`/fontstash (ask)

## Completed

See `todo_completed.md` — every finished step, newest first: the S1–S4 sub-step notes (the long "Done as"
detail), the whole of **S5 (S5.1–S5.8 — the driver step S5.4 carried S5.5/S5.6/S5.7/S5.8 with it)**, the
whole of **S6 (S6.1–S6.4, landed as one commit — the pure half met no separate bar before the driver half
the same day)**, and the dated log entries (the "Bar met" / "Honest boundary" records). Newest entry:
**2026-10-25 — S15, the depth attachment and the opaque pipeline variant** (`src/xud/xud_renderpass.h`/`xud_renderpass.c`
grew a second attachment row and the first `VkImage` in this tree — its own `VkDeviceMemory` carve, its own `VkImageView`,
the pure `xud_renderpass_fill_depth_info`/`xud_renderpass_fill_depth_view_info` half and the `xud_renderpass_retire()`
teardown order; `xud_pipeline_fill_state()`'s depth pair went OFF → ON against it, which makes that pipeline the opaque
variant of the plan's pair; `xud_frame_draw()` now transitions TWO images per frame off one `depthLayout` record; and
`XUD_ALLOCATOR_ALIGN_MAX` went 256 → 65536 because an image's `VkMemoryRequirements` are not a buffer's — the counts
moved to 2147 headless / 2675 GPU, see the entry below). One step before it: **2026-10-25 — S14, the test split**
(`tests/` is 19 files now, not one: `xud_test_harness.h`/`xud_test_harness.c` + `xud_test_main.c` + 17 area TUs, one
per layer rung; the old 5092-line `tests/xud_test.c` is gone, no `src/xud/` file was touched, and the counts are
unchanged at 2108 headless / 2588 GPU — and the clang build caught one real uninitialized-record bug the single-file
run had been passing by luck, see the entry below). One step before it:
**2026-10-25 — S13, the descriptor-layout rung** (`src/xud/xud_descriptor.h`/`xud_descriptor.c`: the two set layouts and
their create-infos inside one record, the pool sized for both descriptor types, set 0 allocated and the per-frame
uniform buffer bound into the allocator's first block, the push-constant range, and the one teardown order that now
runs through it). One step before it: **2026-10-25 — S12, the GPU-memory allocator rung** (`src/xud/xud_allocator.h`/`xud_allocator.c`:
the block array, the free list over `(memoryTypeIndex, size, alignment)`, the transient ring, the memory-type rule read
off the caps' recorded memory rows, and the one teardown order that now runs through it). One step before that:
**2026-10-25 — S11, the first triangle end to end** (`src/xud/xud_shader.h`/`xud_shader.c` + `xud_pipeline.h`/`xud_pipeline.c`
+ `xud_renderpass.h`/`xud_renderpass.c` + `xud_frame.h`/`xud_frame.c` + `xud_renderer.h`/`xud_renderer.c`: the built-in
SPIR-V, one graphics pipeline, one `VkRenderPass` + one `VkFramebuffer` per swapchain image, one command buffer, acquire/submit/present
with an acquire semaphore + one fence per image, and the renderer rung `xud_init_context()` wires). One step before that:
**2026-10-24 — S10, the handle layer** (`src/xud/xud_handle.h`/`xud_handle.c`: the one `XudHandle` type in `xud.h`, the
per-kind slot arrays, the three-part validation, reuse-first allocation with a generation bump and the empty-table state
the context owns). One step before that: **2026-10-24 — S9, the swapchain rung** (the `xud_swapchain.h`/`xud_swapchain.c`
pair, the three surface queries, the selection rules, the recreate-on-resize path and the one teardown order that now runs
through it). One step before that: **2026-10-24 — S8, the context ownership step** (`xud_init_context()` completes to the
surface, `xud_close_context()` is the one teardown order), and before that **2026-10-23 — S7, the GLFW window + surface
rung**. Nothing is finished after S15 — and the test-scaffolding item that was open in `Now` before S11 completed with
S11 rather than as a step of its own: the in-run gate is the diagnostics layer's
message counter read as an assertion (`errorMessages == 0` on the context's own struct), and the failure shapes of
the frame loop are asserted against the live objects of the GPU run — see the S11 entry above for both.

- 2026-10-25 — **S13 the descriptor-layout rung** (`src/xud/xud_descriptor.h` + `src/xud/xud_descriptor.c`, and the
    `xud_pipeline.h`/`xud_pipeline.c` + `xud_renderer.h`/`xud_renderer.c` + `xud_core.h`/`xud_core.c` integration) — see the
    *Steps complete (S13 …)* section at the top of `todo_completed.md` for the full note: the design sketch's descriptor
    discipline becomes a rung **above the allocator and below the renderer**, and it is **context-owned**, because a
    `VkBuffer` is created from memory the allocator owns and is named by the pipeline layout the renderer builds — the
    order in is allocator → descriptor → renderer and the order out is the exact reverse. The **pure** half (the caps gate,
    the two `VkDescriptorSetLayoutCreateInfo` rows and the binding rows they point at, the push-constant range, the pool
    info and its two `VkDescriptorPoolSize` rows, the set-0 allocate info, the buffer info, the write that binds the
    buffer, the uniform block's bytes) calls no driver function and is what the headless run exercises completely; the
    driver half is `xud_descriptor_open`/`xud_descriptor_close` over ten rows of Volk's device table, **no loader slot
    added**. Set 1's **layout** is declared and its **set** is not allocated — the texture step owns the samplers it
    would hold — and nothing calls `vkCmdBindDescriptorSets` or `vkCmdPushConstants` yet: the built-in SPIR-V still
    reads no binding and takes no push constant, so the first triangle is still three vertices from `gl_VertexIndex`.

- 2026-10-25 — **S12 the GPU-memory allocator rung** (`src/xud/xud_allocator.h` + `src/xud/xud_allocator.c`, and the
    `xud_core.h`/`xud_core.c` integration) — see the *Steps complete (S12 …)* section at the top of `todo_completed.md`
    for the full note: the one GPU-memory owner lands between the swapchain and the renderer and the last `void*` sentinel
    in `XudContext` becomes a typed `XudAllocator*`. The **pure** half (open, memory-type selection against the caps'
    recorded memory rows, `VkMemoryAllocateInfo` assembly, the free-list search, release + coalesce, the range accessor,
    the ring rewind, the stats) calls no driver function and is exercised completely by the headless run; the driver half
    is `xud_allocator_acquire`/`xud_allocator_ring_acquire`/`xud_allocator_close`, and `vkAllocateMemory`/`vkFreeMemory`
    are two rows of Volk's device table, already filled in bulk by `xud_loader_bind_device()`, so **no loader slot was
    added**. `vkFreeMemory` returns **void**, so teardown follows the zero-state-before-call rule `vkDestroyDevice`/`vkGetDeviceQueue`
    do, and `xud_allocator_close()` runs `xud_device_wait_idle()` first because a submission or a presentation may still be
    reading the very blocks being freed.

- 2026-10-25 — **S11 the first triangle end to end** (`src/xud/xud_shader.h` + `src/xud/xud_pipeline.h` + `src/xud/xud_renderpass.h`
    + `src/xud/xud_frame.h` + `src/xud/xud_renderer.h`, their five `.c` files, `shaders/solid.vert`/`shaders/solid.frag` and the
    CMake shader step) — see the *Steps complete (S11 …)* section at the top of `todo_completed.md` for the full note: five new
    rungs land between the device and the public API and the context wires the top one. `shaders/` is built, never committed
    (`glslangValidator -V` → `build/gen/*.spv` → `spirv-val` in the same rule → `scripts/spv_to_header.cmake` → `build/gen/xud_solid_spv.h`
    of two `uint32_t` arrays, and a missing tool is a `FATAL_ERROR`); the shader rung checks the SPIR-V header against the **spec's**
    rule (fixed 24 bits + a version byte in `0x00..0x0B`, not glslang's exact magic) and owns one `VkShaderModule` per stage with the
    words **borrowed**; the pipeline rung owns the two modules + `VkPipelineLayout` + `VkPipeline` (dying pipeline → layout → modules,
    `pName` = `"main"`, zero descriptor sets / push-constant ranges / vertex bindings because those rungs are below it and the shaders
    generate their three vertices from `gl_VertexIndex`); the render-pass rung is the **explicit** path the 2026-10-22 decision settles
    — one `VkRenderPass` of one subpass with **one colour attachment row** (no depth attachment: that needs a `VkImage` + `VkDeviceMemory`,
    i.e. the allocator rung that is still not in the tree, which is why depth test/write are `VK_FALSE`) and one `XudFramebuffer` + `VkImageView`
    **per swapchain image**; the frame-loop rung owns one `VkCommandPool` + one `VkCommandBuffer`, one acquire semaphore and **one `VkFence`
    per swapchain image** (not per frame), records the acquire gate as the submit's `pWaitSemaphores` + `pWaitDstStageMask` pair — `vkWaitSemaphores`
    is core 1.2 and this library's instance is at the core-1.0 floor, so that row is nil here — and tracks one `VkImageLayout` per image so the
    layout-transition barrier before `vkCmdBeginRenderPass` names a real `oldLayout`; the renderer rung owns all four and is what `xud_init_context()`
    opens (after the swapchain) and `xud_close_context()` closes (second, after the handle table). **No loader slot was added** — the renderer's names
    are rows of Volk's `VolkDeviceTable`, already filled in bulk by `xud_loader_bind_device()` (which requires the `instanceTable.vkGetDeviceProcAddr`
    row it resolves them through), so the plan's "`vkGetDeviceProcAddr` becomes a loader slot here" was satisfied by the Volk step and what this step
    adds is the row check before every call. Bar met: headless **1916 checks / 0 failures** (was 1739), GPU **2272 / 0** (was 2068), validation-clean
    with `VK_INSTANCE_LAYERS` unset and forced, sanitizers reporting **only** the external allocations the S7/S8/S9/S10 blocks record, clang build
    warning-free at the same counts, `ar -t build/libxud.a` at **37** objects, `nm -u`/`ldd` still Vulkan- and GLFW-free, `scripts/format.sh --check` clean.
    that makes it owned) — see the *Steps complete (S10 …)* section at the top of `todo_completed.md` for the full
    note: `xud.h` grows the public currency (`XudHandle` = three `uint32_t`, 12 B no padding, asserted beside the
    type; `XUD_HANDLE_KIND_NONE`/`TEXTURE`/`FONT`/`MESH`/`MATERIAL`/`MODEL`/`MAX`/`COUNT`), and `xud_handle.h` owns
    the storage (`XudHandleSlot` = `{ occupied, generation, kind, record }` with `record` still the NULL sentinel of
    a rung that is not in the tree, and `XudHandleTable` = one array per kind indexed by the kind value, each array
    NULL until that kind's first allocation). Seven pure functions — `xud_handle_table_open()` / `xud_handle_validate()`
    / `xud_handle_equal()` / `xud_handle_alloc()` / `xud_handle_free()` / `xud_handle_table_live_count()` /
    `xud_handle_table_close()` — and the load-bearing half is the **validation order**: expected-kind range → array
    exists → index below the high mark → generation equality → tag equality → live bit, with the expected kind being
    the *entry point's* kind (that is what makes a cross-kind handle a caught error rather than a read of the wrong
    array) and the live bit last (generation and tag can both match a **free** slot for a handle a caller fabricated).
    Allocation is reuse-first with a generation bump, growth is the doubling sequence the rest of this tree uses
    (`XUD_HANDLE_SLOT_MIN_CAPACITY` 16 → `XUD_HANDLE_SLOT_MAX_CAPACITY` 4096, two asserts make it land on the bound
    exactly) and a slot wanted at the bound is `XUD_OUT_OF_RESOURCES`, not a quiet cut; `xud_handle_free()` validates
    against the handle's **own** tag, so a stale/cross-kind/fabricated handle is refused rather than double-released.
    `xud_core.h` grows a typed `XudHandleTable*` field and its teardown comment grows a handle-table step **before**
    the swapchain (the table is the top of the tree: a live table can name objects the rungs below are about to retire),
    `xud_core.c` opens it after the swapchain and closes it first, and `allocator` stays the one sentinel field left.
    Bar: gcc **and** clang syntax-clean over the whole tree, `./scripts/format.sh --check` clean, headless **1739/0**
    (was 1642) and GPU **2068/0** (was 1969) validation-clean with `VK_INSTANCE_LAYERS` unset **and** forced to
    `VK_LAYER_KHRONOS_validation`, sanitizers reporting **only** the external allocations AGENTS.md records (headless
    72 B / 3, GPU 1136 B / 58 — byte-for-byte the S9 step's numbers, so nothing here leaks; with leak detection on,
    LSan's exit path discards the test binary's buffered stdout, so the counts were read with
    `ASAN_OPTIONS=detect_leaks=0`), `ar -t build/libxud.a` lists **32** objects with `xud_handle.c.o` among them, and
    `nm -u` lists no `vk*`/`glfw*` symbol while `ldd` lists no `libvulkan`/`libglfw`/`libGL`/`libEGL` — the layer
    includes no Vulkan header and calls no driver function at all. **Honest boundary**: the handle currency and the
    slot arrays are in the archive and tested, but a slot's `record` is the NULL sentinel of a rung that does not
    exist — no allocator, no render pass, no framebuffer, no batch, so **no handle names an object yet** — and `xud.h`
    still declares no create/close function that takes a handle (the public API stays narrow on purpose: the type and
    its tag values are public because a caller holds them, the table and these seven functions are internal). That is
    the *Now* item above.

- 2026-10-23 — **S6.6 the vendored GLFW tree built into the archive (Wayland backend)** — see the *Steps complete
    (S6.6 …)* section at the top of `todo_completed.md` for the full note: `CMakeLists.txt` reads upstream's
    `glfw/src/CMakeLists.txt` for `GLFW_BUILD_WAYLAND=ON` and compiles that source set itself as the OBJECT target
    `glfw_vendor` (`-D_GLFW_WAYLAND -D_DEFAULT_SOURCE`, `HAVE_MEMFD_CREATE` from `check_function_exists`), the 9 Wayland
    protocol pairs are generated by `wayland-scanner` into `build/glfw-wayland/`, the vendored TUs drop `-Wextra`/`-Wpedantic`
    (GLFW 3.5.1 is a hard error under both) and keep `-Wall -Werror`, and the four Wayland libs are attached PUBLIC to `xud`
    because an OBJECT library propagates none of its own link deps. Bar: gcc **and** clang builds green from scratch, headless
    **472/0** and GPU **623/0** (unchanged — no test calls GLFW yet), validation-clean both ways, sanitizers green, `ar -t
    build/libxud.a` lists **29** objects, `nm -u` lists no `vk*`/`glfw*` symbol and `ldd` lists no `libvulkan`/`libglfw`/`libGL`/`libEGL`.
    **Honest boundary**: the tree builds and links, nothing in `src/xud/` includes `GLFW/glfw3.h` yet — no window, no surface,
    that is S7.

- 2026-10-23 — **S6.5 the loader rework onto vendored Volk** — see the *Steps complete (S6.5 …)* section
    at the top of `todo_completed.md` for the full note: the 20-slot hand-maintained PFN table is gone, `XudLoader`
    is now a thin per-context wrapper over Volk's `struct VolkInstanceTable` + `struct VolkDeviceTable` plus three
    loader-level rows, `xud_loader_resolve`/`xud_loader_bind`/`XudPfnBits`/`XUD_LOADER_SLOT_COUNT` are gone, bound
    state is handle equality (`xud_loader_instance_bound`/`xud_loader_device_bound`), `volk.h` comes before
    `<vulkan/vulkan.h>` in every Vulkan-bearing TU, and `src/external/volk.c` is in the archive (8 objects) with
    the link still Vulkan-free. Bar: headless **472/0**, GPU **623/0** validation-clean both ways, sanitizers
    green, clang+gcc syntax-clean, `./scripts/format.sh --check` clean.

- 2026-10-18 — **S6 the device layer** (`xud_device.h` + `xud_device.c`): `XudDevice`
owns the `VkDevice` handle, the one `VkDeviceQueueCreateInfo` row built from the physical layer's chosen
family (`queueCount = min(family row, XUD_DEVICE_QUEUE_MAX_COUNT)` 2, stamped, priorities in the struct's
own array so the row's pointer cannot dangle past the call), the `VkPhysicalDeviceFeatures` handed to the
driver is `xud_physical_record_features()`'s **intersection** (never `caps.features.features` verbatim —
`geometryShader` stays off on a driver that has it), the enabled extensions come only from the recorded
196 (`VK_KHR_SWAPCHAIN` at row 84 → exactly one enabled here), `VkDeviceCreateInfo.pNext` stays NULL (the
caps' three `*2` chains came back empty); `XudQueue` is a **record**, not an owner (no `vkDestroyQueue` —
grep zero hits), the two roles `XUD_QUEUE_ROLE_GRAPHICS`/`PRESENT` are a library decision (no `VK_QUEUE_PRESENT_BIT`,
see the S5.2 grep) and both resolve to the same handle here (one family, one queue). `xud_device_wait_idle`
is the teardown guard (the plan's `xud_wait_idle`, layer-prefixed), `xud_device_close` is guard → destroy →
free and a **lost** device is still destroyed. The four device slots were already in the table since S2 —
`XUD_LOADER_SLOT_COUNT` stays **20** — and all four resolve `XUD_OK` through a live `VkInstance` (`XUD_UNSUPPORTED`
with `VK_NULL_HANDLE` in hand), so they are cached, and `vkGetDeviceProcAddr` is not slotted or called here.
The validation layer is what catches the shapes the driver accepts silently (`pQueuePriorities is NULL`,
`queueFamilyIndex (99) is not one of the queue families given via VkDeviceQueueCreateInfo`, `queueIndex (1)
is not less than the 1 queues requested`, total-queue-count-over-available, duplicate family rows, unstamped
create-infos, `queueCount must be greater than 0`); unadvertised extension → `-7`, absent feature → `-8`,
both `XUD_UNSUPPORTED`. Bar: gcc+clang syntax-clean over the whole tree, headless **465/0** (was 361), GPU
**615/0** (was 463) and validation-clean with `VK_INSTANCE_LAYERS` unset and forced on, sanitizers green
both ways, `ar -t build/libxud.a` lists **7** objects with `xud_device.c.o`, link still Vulkan-free. **Honest
boundary**: nothing in the context calls the device layer yet — `xud_core.h`'s `device` field is still a `void *`
sentinel and `xud_init_context` still answers `XUD_UNSUPPORTED`; that wiring is S8's ownership step.
