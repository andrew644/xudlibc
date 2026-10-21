# xudlibc — TODO

Progress tracker. Move items between sections as work happens and add a dated one-liner.
Finished items and their long notes move to `todo_completed.md` (newest first) — never delete a finished
step, just move it, so the active tracker stays short and the log stays complete.

## Now

- [ ] **S6 Device + queues**: one queue family chosen via `vkGetPhysicalDeviceQueueFamilyProperties2` (graphics +
    present), one `VkQueueCreateInfo`, `DeviceCreateInfo` carrying **only** verified features/extensions
    (the `XudDeviceCaps` the physical layer recorded is what it carries — the intersection is already normalised to
    VK_TRUE/VK_FALSE, and the caps' three `pNext` fields are NULL on this driver, so the chain stays empty),
    `xud_wait_idle` guard for teardown. GPU-gated test.
    Facts already probed for S6 (2026-10-18, in `todo_completed.md`'s S5.4 note): 1 device, `apiVersion = 4211042`,
    **196** device extensions with `VK_KHR_SWAPCHAIN` among them, **1** queue family (`queueFlags = 0x1f`,
    `queueCount = 1`), `maxPushConstantsSize` 128, `maxCombinedImageSamplerDescriptorCount` 1.

- [ ] S7 GLFW window + surface: `glfwCreateWindowSurface` (GLFW 3.5.1 — verified present), `glfwGetRequiredInstanceExtensions`
    merged into the instance extension list, `glfwGetPhysicalDevicePresentationSupport` checked against the chosen
    queue family (this is the **present** half of AGENTS.md's one-family rule — there is no `VK_QUEUE_PRESENT_BIT`
    in these headers, see the S5.2 grep), `vkDestroySurfaceKHR` on teardown (no `glfwDestroyVulkanSurface` in 3.5).
- [ ] S8 Ownership + teardown: the context owns loader/instance/device/surface/queues; destroy order device → surface →
    instance; `goto fail` ladder leaks nothing on partial init; a GPU test that creates then destroys and stays validation-clean.
- [ ] S9 Docs: file-top comments naming the layer each file owns; one `todo.md` note per step.

## Next

- [ ] Swapchain creation + recreate on window resize (never leak the old one)
- [ ] First triangle end to end: SPIR-V built by `glslangValidator`, one graphics pipeline, one command buffer,
    acquire/submit/present with semaphores + per-image fence
- [ ] Test scaffolding hardened: headless pure-logic tests green by default, GPU tests opt-in via `XUD_GPU_TESTS=1`,
    validation-layer output treated as a failure signal

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

See `todo_completed.md` — every finished step, newest first: the S1–S4 sub-step notes (the long "Done as"
detail), the whole of **S5 (S5.1–S5.8 — the driver step S5.4 carried S5.5/S5.6/S5.7/S5.8 with it)**, and the
dated log entries (the "Bar met" / "Honest boundary" records). Newest entry: **2026-10-18 — the physical
layer's driver half (S5.4), the layer landing in the archive**. Nothing is finished after that date.

- 2026-10-18 — **S5.4 the physical layer's driver half** (`xud_physical_query_devices` + `xud_physical_open` in
    `src/xud/xud_physical.c`): the physical layer is now complete, in the archive (6 objects), and called by
    `xud_init_context`. Probed on lavvk through the validation layer: 1 device, `apiVersion = 4211042`,
    `INTEGRATED_GPU`, 196 device extensions (`VK_KHR_swapchain` among them), 1 queue family (`queueFlags = 0x1f`),
    `maxPushConstantsSize` 128, `maxCombinedImageSamplerDescriptorCount` 1, `wideLines` absent so the ∩ rule leaves
    it off. The validation layer turned an unstamped `VkQueueFamilyProperties2` row into a hard error — the queue
    query stamps the whole capacity before the call. Bar: gcc+clang syntax-clean, headless **361/0**, GPU **463/0**
    and validation-clean, sanitizers green both ways, link still Vulkan-free. S5.5 (the `XudPhysicalDevice *`
    context field + teardown rung), S5.6 (the archive line), S5.7 (`xud_test_gpu_physical` + the headless
    negative test) and S5.8 (AGENTS.md's *Verified physical-layer driver facts* block + the file-tops) landed
    with it, each as its own note in `todo_completed.md`.
