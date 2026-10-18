# xudlibc — TODO

Progress tracker. Move items between sections as work happens and add a dated one-liner.
Finished items and their long notes move to `todo_completed.md` (newest first) — never delete a finished
step, just move it, so the active tracker stays short and the log stays complete.

## Now

- [ ] **Scaffold + Vulkan bring-up** — broken into ordered, commit-sized steps 2026-10-16. Take them in order, one commit per step, `gcc -fsyntax-only` clean and `build/xud_test` green before each. **S1–S4 done** (repo skeleton, loader bring-up, instance layer, diagnostics layer) — their notes are in `todo_completed.md`.

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

## Completed

See `todo_completed.md` — every finished step, newest first: the S1–S4 sub-step notes (the long
"Done as" detail) and the dated log entries (the "Bar met" / "Honest boundary" records). Newest entry:
2026-10-16 — diagnostics layer (S4). Nothing is finished after that date.
