# xudlibc — TODO

Progress tracker. Move items between sections as work happens and add a dated one-line note.

## Now

- [ ] **Scaffold + Vulkan bring-up** — broken into ordered, commit-sized steps 2026-10-16. Take them in order, one commit per step, `gcc -fsyntax-only` clean and `build/xud_test` green before each.

      - [ ] S1 Repo skeleton: `src/xud/xud.h` (public header, version macros, `XudResult` error enum, `XUD_VALIDATE`/`XUD_GPU_TESTS` config macros), `src/xud/xud_core.c` (context struct, `xud_init_context`/`xud_close_context` stubs), `tests/xud_test.c` (tiny assert-based harness, headless), `CMakeLists.txt` (static lib target + test target + example target). No Vulkan calls yet — the test must pass with no driver.
      - [ ] S2 Loader bring-up: `dlopen("libvulkan.so.1")` + `dlsym("vkGetInstanceProcAddr")`, resolve the entry points we need into an internal PFN table, `xud_vulkan_supported()`. **No `-lvulkan` on the link line.** Headless test: table populated flag is true, and a nil PFN is never called.
      - [ ] S3 Instance: `VkInstanceCreateInfo` + `ApplicationInfo` (`apiVersion = VK_API_VERSION_1_0` floor), `ppEnabledExtensions` only for what `vkEnumerateInstanceExtensionProperties` advertised, `Result` → `XudResult`, destroy on every partial-failure path. GPU-gated test.
      - [ ] S4 Validation layers, debug-gated (`XUD_VALIDATE`): `vkEnumerateInstanceLayerProperties`, request **everything advertised** (never a hardcoded name), recreate the instance with `ppEnabledLayerNames`, attach `DebugUtilsMessengerCreateInfoEXT` for the log side, and `VkValidationFeaturesEXT` on the `pNext` chain where the loader accepts it. Note: `VK_VALIDATE_*` core features are **not in these headers** (SDK-only) — grep before using one.
      - [ ] S5 Physical device: `vkEnumeratePhysicalDevices`, pick one (first for now, policy later), `vkGetPhysicalDeviceProperties2` + `vkGetPhysicalDeviceFeatures2` + `vkEnumerateDeviceExtensionProperties` → record into `XudDeviceCaps` **before** anything enables an extension. Read the real `apiVersion` back. Headless test on the caps-assembly + selection logic (feed a synthetic caps struct).
      - [ ] S6 Device + queues: one queue family chosen via `vkGetPhysicalDeviceQueueFamilyProperties2` (graphics + present), one `VkQueueCreateInfo`, `DeviceCreateInfo` carrying **only** verified features/extensions, `xud_wait_idle` guard for teardown. GPU-gated test.
      - [ ] S7 GLFW window + surface: `glfwCreateWindowSurface` (GLFW 3.5.1 — verified present), `glfwGetRequiredInstanceExtensions` merged into the instance extension list, `glfwGetPhysicalDevicePresentationSupport` checked against the chosen queue family, `vkDestroySurfaceKHR` on teardown (no `glfwDestroyVulkanSurface` in 3.5).
      - [ ] S8 Ownership + teardown: the context owns loader/instance/device/surface/queues; destroy order device → surface → instance; `goto fail` ladder leaks nothing on partial init; a GPU test that creates then destroys and stays validation-clean.
      - [ ] S9 Docs: file-top comments naming the layer each file owns; one `todo.md` note per step.

## Next

- [ ] Swapchain creation + recreate on window resize (never leak the old one)
- [ ] First triangle end to end: SPIR-V built by `glslangValidator`, one graphics pipeline, one command buffer, acquire/submit/present with semaphores + per-image fence
- [ ] Test scaffolding hardened: headless pure-logic tests green by default, GPU tests opt-in via `XUD_GPU_TESTS=1`, validation-layer output treated as a failure signal

## Later

- [ ] GPU memory allocator + suballocation (one allocator, ring for transient; no `vkCreateBuffer` per frame)
- [ ] Descriptor layout: set 0 per-frame uniforms, set 1 sampler array, push constants for model matrix + material/texture index
- [ ] Batch layer (`xudrl`): dynamic vertex/index buffers, flush on pipeline/material change, fixed pipeline set
- [ ] Shapes module (`DrawCube`, `DrawSphere`, `DrawTriangle*`, lines, points)
- [ ] Camera + transform math (3D first; 2D scope is an open decision)
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
- Window/input → **system GLFW 3.5.1** (`pkg-config glfw3`), which in 3.5 gives us `glfwCreateWindowSurface`, `glfwGetPhysicalDevicePresentationSupport`, `glfwGetRequiredInstanceExtensions`, `glfwInitVulkanLoader`. No `glfwDestroyVulkanSurface` — destroy the surface ourselves.
- Vulkan access → **`dlopen` the loader, no `-lvulkan` link dependency** (Volk-style).
- Validation layers → **on in debug**, requested by enumerating advertised layers, never by name.
- Synchronization → **classic** (semaphores + per-image fences + pipeline barriers + subpass deps), not `VK_KHR_vulkan_memory_model`.
- Shaders → **offline SPIR-V only** (`glslangValidator` 11:16.4.0 / `glslc` 2026.3, `spirv-val` in CI). No runtime GLSL.
- Core floor → **`VK_API_VERSION_1_0` + `VK_KHR_SWAPCHAIN`**, everything higher gated on recorded caps.
- GPU memory discipline → **allocate once, suballocate always**, staging via transfer ops.

## Blocked / needs a decision

- 2D vs 3D scope (ask)
- Render pass style: explicit render passes/framebuffers vs `VK_KHR_dynamic_rendering` vs both gated on caps (ask)
- GPU allocator: vendor VMA vs own block allocator (ask)
- Loader: hand-rolled PFN table vs vendor Volk (ask)
- Model format: vendor cgltf (glTF 2.0, what raylib does) vs minimal `.glb` reader (ask)
- Audio: vendor miniaudio vs no audio (ask)
- Text/font: own bitmap atlas vs vendored `stb_truetype`/fontstash (ask)
- Public API handles: index+generation handles vs raylib-style pointers (ask)

## Done

- 2026-10-16 — Commit policy sharpened into a four-point **Commit rule** in `AGENTS.md` (todo item ticked/moved + syntax check and tests green + nothing papered over + docs match the tree ⇒ commit immediately, one commit per step, never a state you are unsure about). First commit made: `.gitignore`, `AGENTS.md`, `todo.md` — `reference/raylib/` stays untracked and gitignored.
- 2026-10-16 — `reference/raylib/` populated: raylib **6.2** headers copied from `~/repos/xudlib/reference/raylib/src/` (git `6.2-5-gc757c743`) — `raylib.h`, `raymath.h`, `rcamera.h`, `rgestures.h`, `rlgl.h`, `config.h` + `LICENSE` + our own `PROVENANCE.md` (560 KB). `.gitignore` added (`bin/`, `build/`, `reference/`), so `git status` stays clean; `AGENTS.md` gained a *Reference material* section naming every file. Docs/data-only — no library code, no build files.
- 2026-10-16 — Repo created (empty, branch `main`, no commits), `AGENTS.md` + `todo.md` written, design sketched (layer stack, frame loop, pipeline set, descriptor layout, vertex format), work broken into S1–S9. Docs-only — no code, no build files yet.
- 2026-10-16 — Toolchain facts verified on this machine and recorded in `AGENTS.md`: gcc 16.2.1 / clang 22.1.8, cmake+ninja+meson, `vulkan-headers` 1.4.357 (`VK_HEADER_VERSION 357`, newest core macro `VK_API_VERSION_1_4`), `libvulkan.so.1.4.357`, `VK_LAYER_KHRONOS_validation` present, ICDs intel/hasvk/nouveau/radeon, GPU = Intel TigerLake-LP GT2 (Iris Xe), `glslangValidator` 11:16.4.0 + `glslc` 2026.3 + `spirv-val`.
- 2026-10-16 — GLFW facts verified against the installed header (`/usr/include/GLFW/glfw3.h`, `pkg-config glfw3` → 3.5.1): the Vulkan surface API **is** present here (`glfwCreateWindowSurface`, `glfwGetPhysicalDevicePresentationSupport`, `glfwGetRequiredInstanceExtensions`) — unlike the Odin sibling, whose vendored bindings lacked surface creation. `#include <GLFW/glfw3.h>` and `#include <vulkan/vulkan.h>` both compile with `gcc -std=c11 -Wall -Wextra -Wpedantic`.
- 2026-10-16 — Header gap confirmed: `VK_VALIDATE_*` / `VkPhysicalDeviceValidationFeatures` are absent from Vulkan-Headers (grep over `/usr/include/vulkan/vulkan_core.h` finds zero hits for `VK_VALIDATION_FEATURES`); only `VkValidationFeaturesEXT` is available. Same finding as the Odin sibling repo.
- 2026-10-16 — Dependency availability checked: **no** Volk, VMA, miniaudio, cgltf, stb, or `cglm` packaged here, and no Vulkan SDK — so every third-party dep is a vendoring decision, and `AGENTS.md` says asking is required before adding one.
