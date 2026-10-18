# xudlibc — TODO

Progress tracker. Move items between sections as work happens and add a dated one-liner.
Finished items and their long notes move to `todo_completed.md` (newest first) — never delete a finished
step, just move it, so the active tracker stays short and the log stays complete.

## Now

- [ ] **S5 Physical device**: `vkEnumeratePhysicalDevices`, pick one (first for now, policy later), `vkGetPhysicalDeviceProperties2` + `vkGetPhysicalDeviceFeatures2` + `vkEnumerateDeviceExtensionProperties` → record into `XudDeviceCaps` **before** anything enables an extension. Read the real `apiVersion` back. Headless test on the caps-assembly + selection logic (feed a synthetic caps struct).

      Broken into S5.1–S5.8 today (2026-10-18) — one commit per sub-step, each meets the four-point
      commit bar on its own. Header facts below were grepped out of `/usr/include/vulkan/vulkan_core.h`
      (Vulkan-Headers 1.4.357) today; the driver numbers are probed when the step that needs them runs.

      - [ ] S5.1 Loader table grows by the **one** slot the physical layer needs: `vkEnumeratePhysicalDevices`,
        `getPhysicalDeviceProperties` (the non-2 form), `getPhysicalDeviceFeatures2`, `getPhysicalDeviceQueueFamilyProperties2`
        and `enumerateDeviceExtensionProperties` are already slots of the instance-level tier; only
        `PFN_vkGetPhysicalDeviceProperties2` is missing (verified: declared at vulkan_core.h:6179, with a
        `...KHR` twin at 10361 that is what `VK_KHR_get_physical_device_properties2` — already enabled on the
        instance by `xud_instance.c` — makes callable at the 1.0 floor). Add that one field, bump
        `XUD_LOADER_SLOT_COUNT` 18 → 19, and keep the "grep -c, keep exact" comment honest. Do **not** resolve
        the *2 queries on demand like the messenger pair: those two are unslotted because they are gated on an
        enabled extension and go stale across an instance recreation, which is not true of these.
        Bar: `xud_loader.h` and every TU that includes it compile clean under the project flags, and
        `XUD_LOADER_SLOT_COUNT` equals the field count.
      - [ ] S5.2 `src/xud/xud_physical.h` — the layer's own header (name it `xud_physical`, not `xud_device`:
        S6 owns `VkDevice` and the two would collide — say so before renaming). `XudPhysicalDevice` = the
        `VkPhysicalDevice` handle (a handle: **never destroyed**, the one rung of the teardown ladder that is
        free-only with no driver call) plus an embedded `XudDeviceCaps` (the name AGENTS.md's Vulkan rules already
        fix): `VkPhysicalDeviceProperties2` and `VkPhysicalDeviceFeatures2` and `VkPhysicalDeviceMemoryProperties2`
        copied in (all three are in these headers, verified at 5809/5803/5843), the advertised **device**-extension
        list as a heap array from the doubling-capacity query (`vkEnumerateDeviceExtensionProperties` has the same
        input/output `pCount` shape as the instance query — verified signature at 4544, and it takes a
        `pLayerName` we pass NULL to for the driver's own list), the chosen queue-family index plus its
        `VkQueueFamilyProperties2` (queried **here**, used by S6 — record once, never re-query), and the `apiVersion`
        read back. Plus the caps-storage macros and their `static_assert`s. Declarations only.
      - [ ] S5.3 The pure half — no driver call, so the headless run covers it completely: `xud_physical_select`
        (given the enumerated handles and their `VkPhysicalDeviceProperties`, pick one; policy stays "first" for now,
        and the reason a better policy is a decision rather than code is that the architecture-id chained struct is
        **not** in these headers (grep `VkPhysicalDeviceArchitectureInfo` → zero hits), so all a discrete-preference
        policy could sort on is core `VkPhysicalDeviceProperties.deviceType` (`VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU`
        /`INTEGRATED_GPU`/`VIRTUAL_GPU`/`CPU`, verified at 2233–2238) plus `vendorID`/`deviceName`/`pipelineCacheUUID`);
        `xud_physical_record_features` (wanted ∩ the driver's `VkPhysicalDeviceFeatures2.features`, an unverified
        feature stays 0 and is never forced on — this is the struct S6 puts into `VkDeviceCreateInfo`);
        `xud_physical_select_queue_family` (a family carrying **both** `VK_QUEUE_GRAPHICS_BIT` and
        `VK_QUEUE_PRESENT_BIT` — AGENTS.md's one-queue-family rule, so a graphics-only family must not be chosen);
        `xud_physical_record_api_version` (read `properties.apiVersion` back, compare with `XUD_INSTANCE_API_FLOOR`,
        answer which of the *2 paths are core at that version and which are extension-at-the-floor — the whole point
        of the floor decision).
      - [ ] S5.4 The driver half: `xud_physical_query_devices` (`vkEnumeratePhysicalDevices`, input/output count
        again → the same doubling-capacity shape as `xud_instance_query_extensions`; probe and record how many devices
        lavvk answers here) and `xud_physical_open(out, loader, instance)` (bind the slots with `xud_loader_bind`
        through the live `VkInstance`, `goto fail` frees what was allocated and destroys nothing). The trap that
        decides the shape: `PFN_vkGetPhysicalDeviceProperties2`, `PFN_vkGetPhysicalDeviceFeatures2` and
        `PFN_vkGetPhysicalDeviceQueueFamilyProperties2` return **void** in these headers (verified: `typedef void
        (VKAPI_PTR *PFN_vkGetPhysicalDeviceProperties)(VkPhysicalDevice, VkPhysicalDeviceProperties *)` at 4536, the
        *2 twins at 6178/6179/6182) — so there is no `VkResult` for `xud_result_from_vk` to map and the only failure
        signal for those three is "the struct still says nothing": every caps field must be at a valid zero state
        **before** the call, and a nil PFN slot is `XUD_UNSUPPORTED`, never called.
      - [ ] S5.5 `xud_core.h`/`xud_core.c`: the `void *physicalDevice` sentinel becomes `XudPhysicalDevice *physicalDevice`
        (the caps live inside it, so the "Pending" comment loses that line and keeps only device/queue/surface/swapchain/allocator);
        `xud_init_context` calls `xud_physical_open(&context->physicalDevice, context->loader, context->instance->instance)`
        between `xud_instance_create` and the `XUD_UNSUPPORTED` return, with a comment saying what `XUD_UNSUPPORTED`
        means here (loader and instance fine, no device enumerable or the *2 queries not reachable at the floor);
        `xud_close_context` frees it in reverse creation order — **before** `xud_instance_destroy`, and free-only, no
        driver call — the comment must say that, since every other rung of that ladder is a driver call. `xud_init_context`
        still returns `XUD_UNSUPPORTED` for valid args: the honest boundary stays until S6.
      - [ ] S5.6 `CMakeLists.txt`: add `src/xud/xud_physical.c` to `add_library(xud STATIC ...)` and extend the comment
        above the target with the physical layer's driver calls (`vkEnumeratePhysicalDevices`, the three void *2 queries,
        `vkEnumerateDeviceExtensionProperties`) — reached only through the table, still no `-lvulkan`, no `-ldl`.
        Bar: `ninja -C build xud` compiles 6 objects, `ar -t build/libxud.a` lists `xud_physical.c.o`, `nm -P` writes
        no undefined `vk*`, `hello_context` still links, still answers `XUD_UNSUPPORTED`, leak-free under the sanitizer config.
      - [ ] S5.7 `tests/xud_test.c`: `xud_test_physical_pure`, headless and synthetic-everything (a fake enumerated list
        + fake `VkPhysicalDeviceProperties` → selection incl. a list whose `deviceType` differs; a fake features struct
        → the ∩ rule incl. a wanted feature the driver lacks staying off; fake queue families incl. a graphics-but-not-present
        one that must **not** be chosen; `apiVersion` at exactly the floor and one below it; caps-storage overflow →
        `XUD_OUT_OF_RESOURCES`) and `xud_test_gpu_physical` (enumerate → pick → record on lavvk: device count, real
        `apiVersion` ≥ floor, `VK_KHR_SWAPCHAIN` among the advertised **device** extensions, a graphics+present family
        found — that last query is the one S6 will use, which is why testing it here is the point). Bar: headless green
        with no device present, `XUD_GPU_TESTS=1` green, `-DXUD_SANITIZERS=ON` green.
      - [ ] S5.8 Docs: AGENTS.md gains a *Verified physical device bring-up* block with the probed numbers (devices
        enumerated, `apiVersion`, device-extension count and whether `VK_KHR_swapchain` is among them, which wanted
        features are present, the queue families and which one has graphics+present); the *Present today* row names the
        new pair; `xud_core.h`'s layer comment matches the tree; file-tops on the new pair name the layer; `hello_context.c`
        file-top bumped to S5; one dated `todo.md` note when the bar is met.

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
