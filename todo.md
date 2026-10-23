# xudlibc — TODO

Progress tracker. Move items between sections as work happens and add a dated one-liner.
Finished items and their long notes move to `todo_completed.md` (newest first) — never delete a finished
step, just move it, so the active tracker stays short and the log stays complete.

## Now

- [ ] Loader rework onto **vendored Volk** (`src/external/volk.h` + `volk.c`, settled 2026-10-23): the 20-slot `XudLoader`
    and `xud_loader_open`/`resolve`/`bind`/`is_open` are either kept as a thin wrapper over Volk's tables or dropped so the
    layers call Volk's `extern PFN_vkXxx vkXxx;` pointers directly; `VK_NO_PROTOTYPES` include order (volk.h **before**
    `<vulkan/vulkan.h>`, or define the macro first) lands here; the instance/physical/device layers move with it.
- [ ] Build the **vendored GLFW** tree (`src/external/glfw`, 3.5.1) into the library on the **Wayland** backend (decided
    2026-10-23): `GLFW_BUILD_WAYLAND` → `-D_GLFW_WAYLAND`, protocol code generated from the 9 `deps/wayland/*.xml` by
    `wayland-scanner` (all requirements verified present here — see AGENTS.md *Toolchain*), exclude the win32/cocoa/macos/wgl/glx/egl/osmesa and `x11_*`/`xkb_unicode.c` halves, keep `-lglfw` off the link line.
- [ ] S7 GLFW window + surface: `glfwCreateWindowSurface` (GLFW 3.5.1 — verified present **in the vendored copy**,
    `glfw/src/vulkan.c` 331), `glfwGetRequiredInstanceExtensions` (264) merged into the instance extension list,
    `glfwGetPhysicalDevicePresentationSupport` (307) checked against the chosen queue family (this is the **present** half
    of AGENTS.md's one-family rule — there is no `VK_QUEUE_PRESENT_BIT` in these headers, see the S5.2 grep),
    `vkDestroySurfaceKHR` on teardown (no `glfwDestroyVulkanSurface` in 3.5, not even in the vendored tree).
- [ ] S8 Ownership + teardown: the context owns loader/instance/device/surface/queues; destroy order device → surface →
    instance; `goto fail` ladder leaks nothing on partial init; a GPU test that creates then destroys and stays validation-clean.
- [ ] S9 Docs: file-top comments naming the layer each file owns; one `todo.md` note per step.

## Next

- [ ] Swapchain creation + recreate on window resize (never leak the old one) — explicit-pass path: the `VkFramebuffer`
    array is per swapchain image and is recreated with the swapchain
- [ ] First triangle end to end on the **explicit `VkRenderPass` + `VkFramebuffer` path** (decided 2026-10-22, see Decided
    below): SPIR-V built by `glslangValidator`, one graphics pipeline, one command buffer, one render pass + one
    framebuffer per image, acquire/submit/present with semaphores + per-image fence; `vkGetDeviceProcAddr` becomes a
    loader slot here (everything below the device resolves through it, not through the instance)
- [ ] Test scaffolding hardened: headless pure-logic tests green by default, GPU tests opt-in via `XUD_GPU_TESTS=1`,
    validation-layer output treated as a failure signal

## Later

- [ ] GPU memory allocator + suballocation (**ours**, ring for transient; no `vkCreateBuffer` per frame — the VMA
    question is settled out 2026-10-23, see Decided below)
- [ ] Descriptor layout: set 0 per-frame uniforms, set 1 sampler array, push constants for model matrix + material/texture index
- [ ] Batch layer (`xudrl`): dynamic vertex/index buffers, flush on pipeline/material change, fixed pipeline set
- [ ] Shapes module (`DrawCube`, `DrawSphere`, `DrawTriangle3D`, `DrawLine3D`, `DrawPoint3D` — 3D only, there is no 2D
    shape family in the API)
- [ ] Camera + transform math (one perspective 3D camera; the 2D scope was settled out 2026-10-23, see Decided below)
- [ ] Textures + samplers (create, upload via staging copies, mipmaps, sampler variants)
- [ ] Mesh / vertex-buffer abstraction + handle table (index+generation)
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
- Public API handles: index+generation handles vs raylib-style pointers (ask)

## Completed

See `todo_completed.md` — every finished step, newest first: the S1–S4 sub-step notes (the long "Done as"
detail), the whole of **S5 (S5.1–S5.8 — the driver step S5.4 carried S5.5/S5.6/S5.7/S5.8 with it)**, the
whole of **S6 (S6.1–S6.4, landed as one commit — the pure half met no separate bar before the driver half
the same day)**,
and the dated log entries (the "Bar met" / "Honest boundary" records). Newest entry: **2026-10-18 — the
device layer, both halves (S6), the layer landing in the archive**. Nothing is finished after that date.

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
