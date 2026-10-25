# xudlibc — agent guidance

Read this before touching anything here. **Work only inside `/home/andrew/repos/xudlibc`** — never edit sibling repos under `~/repos`

## What xudlibc is

- A **C11** library modelled on raylib's public API, but rendered with **Vulkan instead of OpenGL**.
- Same shape as raylib: window/surface creation, `BeginDrawing`/`EndDrawing`, draw calls (`DrawCube`, `DrawMesh`, `DrawTriangle3D`, …), camera + transform math, textures, fonts, models, audio, input — implemented on explicit Vulkan pipelines.
- **No OpenGL, no EGL, no fixed-function, no state machine.** If a raylib feature only makes sense in fixed-function GL, re-express it as a pipeline + a batch, or drop it.
- raylib is a **reference for API shape only** (names, semantics, module split). Never link it, never `#include` it.
  The headers are **already here**: `reference/raylib/raylib.h` (raylib **6.2**, `6.2-5-gc757c743`) — see *Reference material* below for the exact file list.

## Repo layout

| Path | What | Rules |
| --- | --- | --- |
| `src/xud/` | Library sources (`xud.h` public header + one `.c`/`.h` per module) | Where all library code lives |
| `tests/` | Unit tests | Headless by default; GPU tests gated by `XUD_GPU_TESTS=1` |
| `examples/` | Runnable demos | Small, one concern each |
| `shaders/` | GLSL sources (`solid.vert`/`solid.frag` present today) | Compiled at build time by CMake into `build/gen/` — never at runtime, never committed as `.spv` |
| `src/external/` | Vendored third-party deps (raylib's own convention) — **present today**: `volk.h`/`volk.c` (Volk) + a full `glfw/` source tree (GLFW 3.5.1) + `PROVENANCE.md` | Settled 2026-10-23 (see *Settled decisions*): **committed with its license files**, never edited in place; **Volk is built in now** (`volk.c` compiled into the `xud` archive, `src/external` on the include path), and so is the **GLFW tree** (the OBJECT target `glfw_vendor`, **Wayland** backend: 21 GLFW objects in the archive, 29 objects total, the four Wayland libs linked PUBLIC into `xud` — see *Verified GLFW Wayland build*) |
| `reference/raylib/` | raylib **6.2** headers (present today, 560 KB) | **NEVER EDIT** — gitignored, never `#include`d, never built against |
| `bin/`, `build/` | Build output | Gitignored, never committed |
| `todo.md` | Project progress tracker (open work only) | Update every session, dated one-liners |
| `todo_completed.md` | Finished-step log, newest first | Where completed items and their long notes move to — never delete a finished step |

**Present today (after the diagnostics + instance + physical + device + window + context-ownership + swapchain + handle-layer + first-triangle renderer + allocator + descriptor-layout steps + the vendored GLFW Wayland build)**: `src/xud/` = `xud.h` + `xud_core.h`/`xud_core.c` (**the owner**, not a shell any more: `xud_init_context()` runs the whole bring-up ladder to the **renderer and the allocator** and `xud_close_context()` is the one teardown order that hands it all back, so the context owns loader / diagnostics / window / instance / physical / device / **swapchain** / handle table / renderer / **allocator** and the surface lives inside the window it owns) + `xud_loader.h`/`xud_loader.c` (the **Volk-backed** loader: one per-context `XudLoader` holding Volk's `struct VolkInstanceTable` + `struct VolkDeviceTable` plus three loader-level rows, `xud_loader_open`/`close`/`is_open`/`instance_bound`/`device_bound`/`bind_instance`/`bind_device` — the hand-rolled 20-slot PFN table this replaced is gone) + `xud_vulkan.h`/`xud_vulkan.c` (shared vocabulary: `VkResult`→`XudResult` funnel + fixed-buffer name helpers, no driver call) + `xud_diagnostics.h`/`xud_diagnostics.c` (the diagnostics layer: advertised layer selection, `VkValidationFeaturesEXT`, the `VK_EXT_debug_utils` messenger + log callback) + `xud_instance.h`/`xud_instance.c` (the `VkInstance` layer: application info, extension selection, create-info assembly, create/destroy) + `xud_physical.h`/`xud_physical.c` (the physical layer: `XudPhysicalDevice` + an embedded `XudDeviceCaps` + the layer's contract, **both halves implemented** — the pure half (selection, feature intersection, the graphics queue-family rule, `apiVersion` read-back, free-only teardown) landed at S5.3, the driver half `xud_physical_query_devices`/`xud_physical_open` at S5.4, and this `.c` is in the archive with them) + `xud_device.h`/`xud_device.c` (the device layer: `XudDevice` owning the `VkDevice` handle, the `VkDeviceQueueCreateInfo` row the driver was handed, the `VkPhysicalDeviceFeatures` it was handed, the enabled-extension names it was handed, and the two role queues it resolved; `XudQueue` is a **record** of a queue, not an owning type — there is no `vkDestroyQueue`; one queue row from the physical layer's chosen family, `VkDeviceCreateInfo` carrying the physical layer's **feature intersection** and only advertised device extensions, the `xud_device_wait_idle` guard, `xud_device_close` = guard → destroy → free, **both halves implemented** — the pure half (row assembly, extension selection, create-info assembly, queue lookup) and the driver half (`xud_device_open`/`xud_device_wait_idle`/`xud_device_close`), and this `.c` is in the archive with them) + `xud_window.h`/`xud_window.c` (the **window rung**, above the instance and below the swapchain: a `GLFWwindow*` plus the `VkSurfaceKHR` `glfwCreateWindowSurface` handed it, `xud_window_open`/`xud_window_close` over GLFW's own create/destroy, `xud_window_surface_create`/`xud_window_surface_close` over GLFW's create and the loader's `vkDestroySurfaceKHR` row, `xud_window_presentation_support` over GLFW's window-system query, `xud_window_required_instance_extensions`/`xud_window_check_instance_support` pure, and the one piece of state in this tree that is not inside `XudContext` — the process-level live-window count that decides when `glfwTerminate()` may run) + `xud_swapchain.h`/`xud_swapchain.c` (the **swapchain rung**, above the device and below the renderer: `XudSurfaceCaps` recording the three surface queries' answers — capabilities plus fixed arrays for the formats and the present modes, all three **instance-table** rows because they take a `VkPhysicalDevice` — and `XudSwapchain` owning the one `VkSwapchainKHR` plus the `VkImage*` array `vkGetSwapchainImagesKHR` handed it (its only heap member, freed and never destroyed — there is no `vkDestroyImage` call in this layer), the three pure selection rules (`xud_swapchain_choose_image_count`/`xud_swapchain_select_format`/`xud_swapchain_select_present_mode`) called from `xud_swapchain_fill_create_info()` so the create-info and the record of it cannot disagree, `xud_swapchain_open`/`xud_swapchain_recreate`/`xud_swapchain_images_get`/`xud_swapchain_close`, every swapchain name a **device-table** row because it takes a `VkDevice`, and `xud_swapchain_close` guarding with `xud_device_wait_idle()` before the destroy) + `xud_handle.h`/`xud_handle.c` (the **handle layer**, above the swapchain and below the renderer: the storage rung for the object kinds a caller can name — `XudHandleSlot` = `{ occupied, generation, kind, record }` and `XudHandleTable` = one slot array per kind, indexed by the kind value itself, each array NULL until that kind's first allocation; seven **pure** functions — `xud_handle_table_open`/`xud_handle_validate`/`xud_handle_equal`/`xud_handle_alloc`/`xud_handle_free`/`xud_handle_table_live_count`/`xud_handle_table_close` — no driver call and no Vulkan header anywhere in the pair, which is what makes the whole rung headless-testable by construction; `xud.h` grows the public currency `XudHandle` (three `uint32_t`, 12 B no padding) and the `XUD_HANDLE_KIND_NONE`/`TEXTURE`/`FONT`/`MESH`/`MATERIAL`/`MODEL`/`MAX`/`COUNT` tag values, while the table and these functions stay internal like every layer below the public API) + `xud_shader.h`/`xud_shader.c` (the **shader rung**: the SPIR-V words CMake generated, checked against the SPIR-V spec's own header rule, one `VkShaderModule` per stage, `xud_shader_open`/`xud_shader_close`, the words **borrowed** — this layer cannot destroy a module it must not create) + `xud_pipeline.h`/`xud_pipeline.c` (the **pipeline rung**: one graphics pipeline owning its `VkPipelineLayout` and its two `VkShaderModule`s, all four dying in one order pipeline → layout → modules; **two set layouts and one push-constant range named off the descriptor rung now** — `xud_pipeline_open()` takes the `const XudDescriptor*` the context owns and `xud_pipeline_fill_layout_info()` refuses one whose set layouts or uniform buffer are not live, then writes `setLayoutCount = XUD_DESCRIPTOR_SET_COUNT` and `pushConstantRangeCount = 1u` off that record — while vertex bindings/attributes stay **zero**, because the first triangle's shaders still generate their three vertices from `gl_VertexIndex` and read no binding) + `xud_renderpass.h`/`xud_renderpass.c` (the **render-pass + framebuffer rung**: the one `VkRenderPass` of one subpass with **one attachment row, the swapchain colour attachment**, and one `XudFramebuffer` **per swapchain image** with its own `VkImageView` of that image; `xud_renderpass_open`/`xud_framebuffer_open`/`xud_framebuffer_array_open` and their closes) + `xud_frame.h`/`xud_frame.c` (the **frame loop rung**: one `VkCommandPool` + one `VkCommandBuffer`, one acquire semaphore, **one `VkFence` per swapchain image**, the `VkSubmitInfo`/`VkPresentInfoKHR` rows, `xud_frame_open`/`xud_frame_draw`/`xud_frame_close`, and the per-image `VkImageLayout` array that makes the layout-transition barrier's `oldLayout` a fact rather than a guess) + `xud_renderer.h`/`xud_renderer.c` (the **renderer rung**, the owner of all four: pipeline → render pass → framebuffer array → frame on the way in, the exact reverse on the way out, and `xud_renderer_draw()` as the one call that records and submits a frame) + `xud_allocator.h`/`xud_allocator.c` (the **allocator rung**, above the swapchain and below the renderer: the one GPU-memory owner — `XudAllocBlock` = one `VkDeviceMemory` the layer created, `XudAllocRange` = one hole or one carve inside one block, `XudMemoryRange` = what a rung above borrows (`(memory, offset, size)` + the `{ blockIndex, rangeIndex, generation }` triple that makes a stale borrow a caught error), `XudAllocator` = the block array + the free list + the transient ring; the **pure** half — `xud_allocator_open`/`xud_allocator_select_memory_type`/`xud_allocator_fill_allocate_info`/`xud_allocator_find_range`/`xud_allocator_release`/`xud_allocator_range_get`/`xud_allocator_ring_reset`/`xud_allocator_stats` — calls no driver function and is what the headless run exercises completely, the driver half is `xud_allocator_acquire`/`xud_allocator_ring_acquire`/`xud_allocator_close`; **`vkAllocateMemory` and `vkFreeMemory` are two rows of Volk's device table, already filled in bulk by `xud_loader_bind_device()`, so no loader slot was added**, and `vkFreeMemory` returns **void**, so teardown follows the zero-state-before-call rule `vkDestroyDevice`/`vkGetDeviceQueue` do) + `xud_descriptor.h`/`xud_descriptor.c` (the **descriptor-layout rung**, above the allocator and below the renderer: the design sketch's descriptor discipline made a rung — `XudDescriptor` = the two `VkDescriptorSetLayout` handles **and their create-infos + binding rows inside it** (so the create-info and the handle of what it created live in one record, the rule every rung above already follows), its `VkPushConstantRange`, its `VkDescriptorPool` + `VkDescriptorPoolCreateInfo` + its two `VkDescriptorPoolSize` rows, its `VkDescriptorSet` array + the `VkDescriptorSetAllocateInfo` that names set 0's layout, its `VkBuffer` + `VkBufferCreateInfo` + the `XudMemoryRange` the allocator handed it + the `VkWriteDescriptorSet`/`VkDescriptorBufferInfo` pair that binds the buffer into set 0, and its `XudFrameUniformRow` of the bytes that row describes; **eight pure functions** — `xud_descriptor_caps_support`/`fill_set_layouts`/`fill_push_range`/`fill_pool_info`/`fill_set_allocate_info`/`fill_buffer_info`/`fill_write_info`/`fill_uniform_block` — and the driver half `xud_descriptor_open`/`xud_descriptor_close`, with `xud_descriptor_retire()` as the one teardown order both the close and the failure path use; **set 1's layout is declared and its set is not allocated**, because the texture step owns the samplers it would hold); `tests/` = `xud_test.c`; `examples/` = `hello_context.c`; `shaders/` = `solid.vert` + `solid.frag` (built by CMake into `build/gen/`: `glslangValidator -V` → `.spv` → `spirv-val` in the same rule → `scripts/spv_to_header.cmake` → `build/gen/xud_solid_spv.h` of `uint32_t` arrays; **nothing SPIR-V is committed**); `reference/raylib/`; `src/external/` (vendored Volk + a GLFW source tree + `PROVENANCE.md`, tracked since 2026-10-23 — **both are built in now**: `src/external/volk.c` is compiled into the `xud` archive and the vendored GLFW tree is compiled in as an OBJECT target on the **Wayland** backend, so `ar -t build/libxud.a` lists **39** objects (`volk.c.o` + 21 GLFW objects + the 17 xud module objects), and every executable that links `xud` links the four Wayland libs and inherits `${GLFW_DIR}/include` on the include path, because `xud_window.h` includes `<GLFW/glfw3.h>` (see the include-order rule in *Verified window + surface facts*); `build/` (gitignored CMake output, plus `build-san/`/`build-clang/` when sanitizers or the second opinion are wanted, and `build/gen/` for the built SPIR-V + its generated header). **Not created yet**: `bin/`. A `VkPhysicalDevice`, a `VkDevice` with its queues, **a GLFW window and a `VkSurfaceKHR`** and **a `VkSwapchainKHR` with its images** are created now, and the **handle currency and its per-kind slot arrays exist now** (S10's handle layer landed on top of S9's swapchain step: `xud_init_context()` opens a table as the last rung of the bring-up ladder and `xud_close_context()` closes it as the first rung of the teardown order, in the archive and tested headless — the layer calls no driver function at all) — and **the renderer rung exists now** (S11, 2026-10-25: `xud_init_context()` opens the renderer after the swapchain and `xud_close_context()` closes it second, and the GPU test run draws frames through it, so a `VkPipeline`, a `VkRenderPass`, one `VkFramebuffer` + `VkImageView` per swapchain image, a `VkCommandPool`/`VkCommandBuffer`, an acquire semaphore and per-image fences are all live there) — and **the allocator rung exists now** (S12, 2026-10-25: `xud_init_context()` opens it after the swapchain and `xud_close_context()` closes it after the renderer and before the swapchain, so **no field of `XudContext` is a sentinel any more**, and the GPU test run allocates, suballocates, grows to a second block, releases, reuses, wraps the ring and frees real `VkDeviceMemory` through it) — and **the descriptor-layout rung exists now** (S13, 2026-10-25: `xud_init_context()` opens it after the allocator and before the renderer and `xud_close_context()` closes it after the renderer and before the allocator, so a context that comes back `XUD_OK` owns **two `VkDescriptorSetLayout`s, a `VkDescriptorPool`, one `VkDescriptorSet` allocated from it and one `VkBuffer` bound to memory the allocator owns** — the first thing above the allocator that ever asked it for a block, so an `XUD_OK` context's allocator holds **one block and one live range** rather than none) — but there is still **no `VkImage`, no vertex stream, no batch and no draw that binds anything**: `xud_descriptor.c` calls neither `vkCmdBindDescriptorSets` nor `vkCmdPushConstants`, the built-in SPIR-V still reads no binding and takes no push constant, and set 1's **layout** is declared while its **set** is the texture step's, which is why **no handle names an object yet** — a live slot's `record` field is still the NULL sentinel of the batch/renderer rung above, and `xud.h` still declares no create/close function that takes a handle (see *Verified descriptor-layout facts (2026-10-25)* for where that boundary is). The context carries **no queue field at all** — `XudQueue` is a record, not an owning type, and both role queues live inside `XudDevice` (see *Verified context ownership facts*); it carries **no surface field either** — the surface lives inside the `XudWindow` it owns (see *Verified window + surface facts*).

## Toolchain (verified on this machine — use exactly these)

| Thing | Value |
| --- | --- |
| OS | Arch Linux, `x86_64`, 8 cores |
| Compilers | `gcc` **16.2.1**, `clang` **22.1.8** |
| Build | `cmake` + `ninja` (also `meson`, `make`, `ccache`, `pkg-config` present) |
| Vulkan headers | `/usr/include/vulkan/vulkan.h`, `vulkan-headers` **1.4.363** (`VK_HEADER_VERSION 363`, newest core macro = `VK_API_VERSION_1_4`) |
| Vulkan loader | `libvulkan.so.1` → `libvulkan.so.1.4.363` (ICD loader, present) |
| Validation layer | `VK_LAYER_KHRONOS_validation` present (`/usr/share/vulkan/explicit_layer.d`, `api_version 1.4.363`) |
| ICDs | `intel_icd`, `intel_hasvk_icd`, `nouveau_icd`, `radeon_icd` |
| GPU | Intel **TigerLake-LP GT2 (Iris Xe)** → `vulkan_intel` (lavvk); `/dev/dri/renderD128` present |
| Window/input | **GLFW 3.5.1, vendored and built in** (`src/external/glfw`, Wayland backend — see *Verified GLFW Wayland build* and *Verified window + surface facts*): 21 GLFW objects sit in the `xud` archive, the four Wayland libs (`wayland-client`/`wayland-cursor`/`wayland-egl`/`xkbcommon`, all present here) are linked PUBLIC into every executable that links `xud`, and there is still **no `-lglfw`** anywhere. The system package (`local/glfw 1:3.5.1-1`, `/usr/include/GLFW/glfw3.h`, `libglfw.so.3.5`) is no longer what the build uses; **`src/xud/xud_window.h` includes `<GLFW/glfw3.h>` now** (after `<vulkan/vulkan.h>` — see the include-order rule in *Verified window + surface facts*), and the GPU test run creates a real window on this machine's Wayland session (`XDG_SESSION_TYPE=wayland`, `WAYLAND_DISPLAY=wayland-1`), so a window and a surface exist there — the headless run still creates neither |
| Wayland (GLFW's backend) | `wayland-scanner` at `/usr/bin` (`local/wayland 1.26.0-1`); `pkg-config` finds `wayland-client`/`wayland-cursor`/`wayland-egl`/`xkbcommon` — the four GLFW 3.5.1 `pkg_check_modules` requires — and `libdecor-0` (`local/libdecor 0.2.5-1`, **unused** by 3.5.1's CMake); libs `libwayland-client.so.0.26.0`, `libwayland-cursor.so.0`, `libwayland-egl.so.1`, `libxkbcommon.so.0.13.2` |
| Shader tools | `glslangValidator` (glslang 11:16.4.0, GLSL 4.60 / ESSL 3.20), `glslc` 2026.3, `spirv-val`, `spirv-opt`, `spirv-as` |
| **Not installed** | No Vulkan SDK, no Volk, no VMA, no miniaudio, no cgltf, no stb, no `cglm` — anything like that must be vendored by hand, and that needs a decision first |

**Verified GLFW 3.5.1 Vulkan surface**:
`glfwVulkanSupported()`, `glfwGetRequiredInstanceExtensions(uint32_t *count)`, `glfwGetInstanceProcAddress(VkInstance, const char *)`,
`glfwGetPhysicalDevicePresentationSupport(VkInstance, VkPhysicalDevice, uint32_t queuefamily)`,
`glfwCreateWindowSurface(VkInstance, GLFWwindow *, const VkAllocationCallbacks *, VkSurfaceKHR *)`, `glfwInitVulkanLoader(PFN_vkGetInstanceProcAddr)`.
There is **no** `glfwDestroyVulkanSurface` in 3.5 — destroy the surface with `vkDestroySurfaceKHR`.

**The vendored copy says the same** (probed 2026-10-23 in `src/external/glfw`): `glfw/src/vulkan.c` has
`glfwGetRequiredInstanceExtensions` (264), `glfwGetPhysicalDevicePresentationSupport` (307), `glfwCreateWindowSurface`
(331); `glfwInitVulkanLoader` is at `glfw/src/init.c` 484, declared at `glfw/include/GLFW/glfw3.h` 2368; `glfw3.h`
says `GLFW_VERSION_MAJOR/MINOR/REVISION` 3/5/1, the same version as the system copy; still no `glfwDestroyVulkanSurface`.

**Verified Volk bring-up (2026-10-23)** — what `src/xud/xud_loader.c` does now, probed today by a scratch program linked against `build/libxud.a` (loader → instance → physical → device, then deleted; the same sequence through the validation layer answers the same):
- `volkInitialize()` answers `VK_SUCCESS` here and `volkGetInstanceVersion()` reads **4211051**; Volk's POSIX bring-up is the same `dlopen("libvulkan.so.1", RTLD_NOW|RTLD_LOCAL)` the hand-rolled table used (volk.c 114, fallback `libvulkan.so` 116), so the no-driver path is unchanged: `volkInitialize()` answers `VK_ERROR_INITIALIZATION_FAILED` and `xud_loader_open()` maps it to `XUD_UNSUPPORTED`.
- After `volkInitialize()` **only** the loader-level names are filled in Volk's globals: `vkCreateInstance`, `vkEnumerateInstanceExtensionProperties`, `vkEnumerateInstanceLayerProperties`, `vkEnumerateInstanceVersion`; `vkEnumeratePhysicalDevices` and `vkDestroyInstance` are still NULL. That is why `XudLoader` copies the three loader-level rows into itself at open time and still requires all three (`XUD_LOADER_GLOBAL_SLOT_COUNT` 3) before it calls itself open — and why the copy exists at all: Volk's globals are **process-wide**, so a second context's `volkFinalize()` would clear rows a first context is still calling through. The layers above keep calling `loader->createInstance`, never `vkCreateInstance`.
- `volkLoadInstanceTable(&table, instance)` fills the whole instance-level table — **127 entries** as compiled here — including `vkDestroyInstance`, `vkDestroySurfaceKHR`, the `VK_EXT_debug_utils` messenger pair and `vkGetDeviceProcAddr`. `volkLoadDeviceTable(&table, device)` fills **710 entries** — `vkDestroyDevice`, `vkDeviceWaitIdle`, `vkGetDeviceQueue`, `vkCmdDraw`, `vkAllocateMemory` among them — **but only through Volk's global `vkGetDeviceProcAddr`**: probed with the global NULL and a live device, the whole device table stays NULL; set the global from `instanceTable.vkGetDeviceProcAddr` first and it fills. `xud_loader_bind_device()` does exactly that, and refuses (without calling) when the loader has no bound instance or the row is NULL — otherwise a second context would fill the first context's device table with the second instance's rows.
- **Check-before-you-use survives at the row level**: a name whose extension is not enabled on the instance answers NULL in the table (probed: with `VK_KHR_swapchain` not enabled, `deviceTable.vkCreateSwapchainKHR`/`vkDestroySwapchainKHR`/`vkGetSwapchainImagesKHR`/`vkAcquireNextImageKHR`/`vkQueuePresentKHR` all came back NULL while the core device rows filled). Every layer still checks the row before calling it and answers `XUD_UNSUPPORTED` rather than calling through NULL.
- `volkFinalize()` clears Volk's globals and `volkGetInstanceVersion()` then reads 0; `xud_loader_close()` calls it and `memset`s both tables before freeing, so a closed loader has no live row anywhere. `xud_loader_bind_instance()` on a new instance **clears the device table** and nulls `boundDevice` — a device table filled for a dead instance is pointers into a dead layer.
- **Bound state is handle equality, not a flag**: `XudLoader` records `VkInstance boundInstance` + `VkDevice boundDevice`, and `xud_loader_instance_bound()`/`xud_loader_device_bound()` compare the handle in hand against the recorded one (both reject `VK_NULL_HANDLE`). This is what makes the old resolve-on-demand rule (the messenger pair) unnecessary: a stale handle across an instance recreation fails the compare before any row is read. `xud_instance_destroy()` re-binds the loader to the instance it is about to destroy when it is not already bound to it — a second instance on one loader leaves the loader bound to the first — and skips the re-bind for a handle-less synthetic instance, which stays a free-only teardown with no driver call.
- **Guard codes, and the headless run covers both**: a bind with no handle in hand is `XUD_INVALID_ARG`; a **device** bind with no instance bound — or with a NULL `instanceTable.vkGetDeviceProcAddr` row — is `XUD_UNSUPPORTED`, since the rung cannot resolve and that is not a bad argument. A refused bind leaves every table row NULL, which is what the headless guard test asserts.
- **Include order is Volk's trap, landed here**: `volk.h` errors `To use volk, you need to define VK_NO_PROTOTYPES before including vulkan.h` when `VULKAN_H_` is already in (volk.h 17–18) and defines the macro itself when it is not (25–26), so **`#include "volk.h"` comes immediately before `<vulkan/vulkan.h>`** in every Vulkan-bearing TU — `xud_loader.h`, `xud_vulkan.h`, `xud_diagnostics.h`, `xud_instance.h`, `xud_physical.h`, `xud_device.h`. `src/external/volk.c` compiles as shipped, clean with gcc **and** clang under `-std=c11 -Wall -Wextra -Wpedantic -Werror`. User code still includes `xud.h` only: a scratch TU including nothing but `xud.h` compiles under the project flags with no `volk.h`, no `VK_NO_PROTOTYPES` and no Vulkan header of its own.
- **No loader slot exists any more** — `XUD_LOADER_SLOT_COUNT`, `XUD_LOADER_NAMES`, `XudPfnBits`, `xud_loader_resolve()`, the per-name `xud_loader_bind()` and `resolvedCount` are gone; a name is a row of one of Volk's two tables, filled in bulk. The older blocks below speak of slots in the vocabulary of the tree as it stood until 2026-10-18; what a step needs today is that the row is non-NULL, and the renderer step adds no slot — it reads `loader->deviceTable.vkGetDeviceProcAddr`-adjacent rows off the table `xud_loader_bind_device()` already filled.

- Test counts after this step: headless **472 checks / 0 failures** (was 465), `XUD_GPU_TESTS=1` **623 / 0** (was 615) and validation-clean with `VK_INSTANCE_LAYERS` unset **and** forced to `VK_LAYER_KHRONOS_validation` (the only `ERROR/validation` line in either run is still the diagnostics layer's own synthetic headless probe), `-DXUD_SANITIZERS=ON` green both ways (472 / 623, no report), `ar -t build/libxud.a` lists **8** objects with `volk.c.o` among them, and neither `nm -u build/xud_test` nor `nm -u build/hello_context` lists a `vk*` symbol while `ldd` lists no `libvulkan` for either.

**Verified GLFW Wayland build (2026-10-23)** — what `CMakeLists.txt` now does with the vendored `src/external/glfw` tree, probed today by building it from scratch with gcc **and** clang and by reading the archive/link (`ar -t`, `nm`, `ldd`); **no window is created anywhere in the tree** and nothing in `src/xud/` includes `GLFW/glfw3.h` yet, so this step is the build, not the surface:
- **The source set is upstream's `glfw/src/CMakeLists.txt` read for `GLFW_BUILD_WAYLAND=ON`, `GLFW_BUILD_X11`/COCOA/WIN32=OFF on Linux**: the always-compiled base set (`context.c init.c input.c monitor.c platform.c vulkan.c window.c` + the four `null_*` halves the `GLFW_PLATFORM` init hint can select at runtime), the Linux/POSIX time/thread/module/poll bits, the Wayland half (`wl_init.c wl_monitor.c wl_window.c` + `wl_platform.h`) and `linux_joystick.c` (gated on `(X11 or Wayland) and __linux__`). `win32_*`/`cocoa_*`/`macos_*`/`nsgl_*`/`glx_context.c`/`x11_*`/`xkb_unicode.c` are not in the build. **`egl_context.c` and `osmesa_context.c` are in, not out**: `wl_window.c` and the `null_*` halves reference `_glfwInitEGL`/`_glfwCreateContextEGL`/`_glfwInitOSMesa`/`_glfwTerminateEGL` **by name**, so leaving them out is a link error, not a smaller binary; neither pulls a GL or EGL library (GLFW `dlopen`s those at runtime, `ldd` lists no `libGL`/`libEGL`) and `glfwCreateWindowSurface` refuses a window whose client API is not `GLFW_NO_API`, so the Vulkan path never runs through them.
- **The Wayland protocol headers/sources are generated at build time** from the 9 XMLs in `deps/wayland/` by `wayland-scanner` into `build/glfw-wayland/` — nothing generated lands next to the vendored sources, which are never edited in place. **Naming trap**: the `-client-protocol-code.h` files are **not compiled as their own TUs** — CMake treats a `.h` source as a header and does not compile it (upstream's own `target_sources(glfw PRIVATE ${header_file} ${code_file})` exists only to force generation), and `wl_init.c` 59–91 `#include`s each `-code.h` inside its own body, so the protocol stubs (`wp_viewporter_destroy`, `xdg_wm_base_add_listener`, `zwp_pointer_constraints_v1_destroy`, …) are local `t` symbols **inside `wl_init.c.o`**, which is what `nm` shows there.
- **Warning mode**: the vendored TUs are **not** compiled in the library's mode. GLFW 3.5.1 is a hard error under `-Wpedantic` (the `dlsym` object↔function-pointer casts in `posix_module.c` 50, the same ISO C rule AGENTS.md records for our own loader) and under `-Wextra` (the unused parameters every `null_*` half and every Wayland protocol callback carries). Upstream asks for `-Wall` only, as C99; here the tree compiles as **C11** with `-Wall -Werror` kept and `-Wextra`/`-Wpedantic` switched off per target (`-Wno-extra -Wno-pedantic`), verified clean with gcc 16.2.1 and clang 22.1.8. `-D_DEFAULT_SOURCE` is upstream's own Linux define (`glfw/src/CMakeLists.txt` 141): `-std=c11` sets `__STRICT_ANSI__`, which would otherwise hide the glibc POSIX extensions `wl_init.c`/`wl_window.c` need; `-D_GLFW_WAYLAND` is upstream's backend define (54–55); `HAVE_MEMFD_CREATE` comes from CMake's own `check_function_exists(memfd_create)` (upstream 66–70) and is present here, so `wl_window.c` 105–106's `memfd_create` shared-memory hint is compiled in.
- **The Wayland link flags are a property of `xud`, not of `glfw_vendor`**: `glfw_vendor` is an OBJECT library, so its objects go into `libxud.a` and there is still exactly one library to link — but an object library's own link dependencies are **not** propagated to the targets that use it (verified both ways here: `glfw_vendor` PRIVATE and `glfw_vendor` PUBLIC both left every executable's link line at `libxud.a` only, with `ldd` at libc only, so a GLFW call would have failed to link) and a static archive records no dependencies of its own. The four Wayland libs are therefore named on `target_link_libraries(xud PUBLIC …)`, from pkg-config's `GLFW_WAYLAND_LDFLAGS`, **not** `GLFW_WAYLAND_LIBS` (verified: that form expanded to nothing — it is only filled for the `_DEBUG`/`_RELEASE` variants — and `_STATIC_LDFLAGS` would add `-lrt`, which is not a library on this machine). `ldd` now lists `libwayland-client.so.0`, `libwayland-cursor.so.0`, `libwayland-egl.so.1`, `libxkbcommon.so.0` and `libffi.so.8` (wayland-client's own dependency) for both executables.
- **The Vulkan-free link rule is unchanged and still holds**: `nm -u build/xud_test` and `nm -u build/hello_context` list no `vk*` and no `glfw*` symbol, and `ldd` lists no `libvulkan`, no `libglfw` and no `libGL`/`libEGL` — there is still no `-lglfw` and no `-ldl` anywhere. `ar -t build/libxud.a` lists **29** objects: the 8 library objects (`volk.c.o` among them) plus 21 GLFW objects (`context.c.o` … `wl_window.c.o`, the four `null_*`, the four `posix_*`, `egl_context.c.o`, `osmesa_context.c.o`, `linux_joystick.c.o`).
- Test counts after this step: headless **472 checks / 0 failures** and GPU **623 / 0** — **unchanged**, because no test calls GLFW yet (the surface test is S7's); validation-clean with `VK_INSTANCE_LAYERS` unset and forced, sanitizers green both ways, and the clang build (`-DCMAKE_C_COMPILER=clang`) is warning-free over the vendored set.

**Verified window + surface facts (2026-10-23)** — what `src/xud/xud_window.h`/`xud_window.c` (S7) state, probed today by the GPU test run (a real window on this machine's Wayland session, a real surface created and destroyed) and by reading the vendored GLFW tree; **no swapchain exists in the tree yet**:
- **The rung is `xud_window`, above the instance and below the swapchain**, and it owns **both halves that must die together**: the `GLFWwindow*` and the `VkSurfaceKHR` `glfwCreateWindowSurface` handed it. `xud.h`'s public surface is unchanged — the window layer is internal, like every layer below the public API, and `XudContext` grows a typed `XudWindow*` field (still NULL: `xud_init_context()` creates no window, wiring it is S8's).
- **GLFW's Vulkan declarations sit inside `#if defined(VK_VERSION_1_0)`** (`glfw3.h` 6342, the block carrying `glfwVulkanSupported` 6346, `glfwGetRequiredInstanceExtensions` 6390, `glfwGetPhysicalDevicePresentationSupport` 6470, `glfwCreateWindowSurface` 6540), so **`<vulkan/vulkan.h>` must be seen before `<GLFW/glfw3.h>`** — the same include-order rule Volk already forces, one rung higher: in `xud_window.h` the order is `"volk.h"` → the xud layer headers → `<vulkan/vulkan.h>` → `<GLFW/glfw3.h>` last. A `xud_window.h` that included glfw3.h first compiles but declares none of those four names, which is a link error, not a message about the reason.
- **`xud_window.h` is the only header of this rung that includes `glfw3.h`, and the include path is PUBLIC on `xud`** (`"${GLFW_DIR}/include"` in `CMakeLists.txt`): the test and example executables link `xud`, so they inherit it and compile `xud_window.h` without naming GLFW's include dir themselves.
- **GLFW's required instance extensions on the Wayland backend are exactly two**, read back by the GPU test: `VK_KHR_surface` and `VK_KHR_wayland_surface`. `VK_KHR_surface` is one of the three the instance layer already wants, so a merged instance enables **4** names, not 5 — the window layer appends GLFW's list and the instance layer's existing "enable nothing that was not advertised" rule does the rest. `XUD_INSTANCE_EXTRA_MAX 3u` is the shared bound: `xud_window.h` sizes its own required-name storage by it, `xud_instance.h` declares the extra-name parameter by it, and both headers `static_assert` that this layer's three wanted names + the diagnostics layer's + the window layer's fit `XUD_MAX_INSTANCE_EXTENSIONS 8u`.
- **The instance must carry GLFW's names before a surface can exist** — the load-bearing fact, and the reason `xud_instance_create()` grew an `extraNames`/`extraCount` parameter at all: `glfwCreateWindowSurface` resolves `vkCreateWaylandSurfaceKHR` through `vkGetInstanceProcAddr` **against the instance it is handed** (`wl_window.c` 145), so the row answers NULL for an instance that does not have the extension enabled. Probed both ways: an instance with only this layer's three wanted names answers `glfwGetPhysicalDevicePresentationSupport` = `GLFW_FALSE` and `glfwCreateWindowSurface` = `VK_ERROR_EXTENSION_NOT_PRESENT` (-7) with no surface; the merged instance answers `GLFW_TRUE` and `VK_SUCCESS`.
- **A `VkPhysicalDevice` belongs to the instance it was queried through**, and the validation layer says so hard: reusing a device queried through instance A after instance B was created answers `vkGetPhysicalDeviceWaylandPresentationSupportKHR: Invalid physicalDevice [VUID-vkGetPhysicalDeviceWaylandPresentationSupportKHR-physicalDevice-parameter]` and the loader **aborts** (core dumped, exit 134). The GPU test therefore creates the merged instance first and queries the physical device through it — the same ownership rule the physical and device tests already follow, one rung up.
- **`glfwCreateWindow` takes `int` extents**, so `XUD_WINDOW_MAX_EXTENT` is `(uint32_t)INT_MAX` (`static_assert`ed against `<limits.h>`) and anything above it is `XUD_INVALID_ARG`, not a wrap; `XUD_WINDOW_MIN_EXTENT 1u` is GLFW's own "zero or negative is invalid" rule read off `window.c`. `XUD_WINDOW_TITLE "xudlibc"` is fixed rather than a public knob this layer does not own (GLFW's Wayland backend uses the title for window identification and libdecor decoration hints).
- **`glfwVulkanSupported()` is GLFW's loader query** (`vulkan.c` 14–16 → `_glfwInitVulkan(_GLFW_FIND_LOADER)`): it finds the Vulkan loader, creates no window and no instance, and answers `GLFW_FALSE` when GLFW cannot find one — which is why `xud_window_open()` answers `XUD_UNSUPPORTED` rather than creating a window it cannot give a surface.
- **`glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API)` is required before `glfwCreateWindow`**: `glfw/src/vulkan.c` 110–113 refuses a window whose `context.client` is not `GLFW_NO_API` with `VK_ERROR_NATIVE_WINDOW_IN_USE_KHR` — a GL/EGL-backed window is not a Vulkan window, and this repo has no GL anywhere.
- **GLFW's state is process-level, so the live-window count is the one piece of state here that is not inside `XudContext`**: `glfwInit()` is idempotent (`init.c` 386–389 answers `GLFW_TRUE` when already initialized) and `glfwTerminate()` is GLFW's own teardown, which walks GLFW's window list, cursors, monitors and mappings (`init.c` `terminate()`). `xudGlfwLiveWindowCount` is incremented only after `glfwCreateWindow` answered with a window, decremented only by `xud_window_close()`, and `glfwTerminate()` runs only when it reaches zero — so the GPU test's two-window pair (open two, close both) exercises exactly the case AGENTS.md's "multiple windows must work" rule is about.
- **Surface teardown is `vkDestroySurfaceKHR` through the loader's instance-table row** — there is no `glfwDestroyVulkanSurface` in 3.5 (not even in the vendored tree, see the *Verified GLFW Wayland build* block). `window->surfaceInstance` records the instance the surface was created for (borrowed, not owned: `vkDestroySurfaceKHR` takes it as its first argument and the extension says that instance must still be alive), `xud_loader_instance_bound()` is required before both create and teardown (rows that are not this instance's rows cannot destroy its surface), and **the destroy row is checked before the create** — a surface this layer cannot destroy is a surface it must not create.
- **Presentation support is GLFW's query, not a loader row**: `glfwGetPhysicalDevicePresentationSupport(instance, physicalDevice, queueFamilyIndex)` — `wl_window.c` resolves `vkGetPhysicalDeviceWaylandPresentationSupportKHR` through `vkGetInstanceProcAddr` — and it **asserts** the instance and the physical device are non-NULL, so a handle-less one is `XUD_INVALID_ARG`, not a call. It is asked about the physical layer's **chosen** queue family (`caps.queueFamilyIndex`, `XUD_PHYSICAL_NO_QUEUE_FAMILY` = never chosen is a bad argument), which is the **present** half of the one-family rule the physical layer's graphics check is the other half of (there is no `VK_QUEUE_PRESENT_BIT`, see the S5.2 grep).
- **`xud_window_close()` refuses a window whose surface is still live** (`XUD_INVALID_ARG`): destroying the window under a live surface would leave a surface that names a dead window, and GLFW's `terminate()` walks its window list. That is the teardown order `xud_close_context()` will have to use — surface → window → instance — and the GPU test asserts the refusal, the surface teardown, and the window teardown in that order.
- **A leak this layer does not own, recorded rather than papered over**: the GPU run under `-fsanitize=address,undefined` reports one leak of 8 bytes in one allocation whose whole stack is outside this tree — `malloc ← g_malloc ← g_strdup ← gdk_set_allowed_backends (libgdk-3.so.0) ← libdecor's GTK plugin ← libdecor_new ← _glfwInitWayland (`src/external/glfw/src/wl_init.c` 877, the `if (_glfw.wl.libdecor.handle)` branch) ← glfwInit`. GLFW `dlopen`s libdecor when it is present (`local/libdecor 0.2.5-1` here) and libdecor's GTK plugin keeps a `g_strdup`ed string forever; nothing in xudlibc allocates, frees, or can see it, so there is nothing to fix and **no suppression is installed** to make the run look clean. The same run is green apart from that one external allocation, and the headless sanitizer run reports nothing.
- Test counts after this step: headless **525 checks / 0 failures** (was 472), `XUD_GPU_TESTS=1` **719 / 0** (was 623) and validation-clean with `VK_INSTANCE_LAYERS` unset **and** forced to `VK_LAYER_KHRONOS_validation` (the only `ERROR/validation` line in either run is still the diagnostics layer's own synthetic headless probe), `-DXUD_SANITIZERS=ON` green headless (525 / 0, no report) and the GPU run reporting only the external libdecor leak above, the clang build (`-DCMAKE_C_COMPILER=clang`) warning-free and same counts, `ar -t build/libxud.a` lists **30** objects with `xud_window.c.o` among them, and `nm -u build/xud_test` still lists no `vk*` and no `glfw*` symbol while `ldd` lists no `libvulkan`, no `libglfw` and no `libGL`/`libEGL` — GLFW's Vulkan half resolves its own entry points through the loader it `dlopen`s, and the window layer never calls a Vulkan entry point except through a loader row.

**Verified descriptor-layout facts (2026-10-25)** — what `src/xud/xud_descriptor.h`/`xud_descriptor.c` (S13)
state, probed today by the headless run (the pure half, with synthetic caps and synthetic live handles)
and by the GPU run (two real set layouts, a real pool, a real set 0, a real `VkBuffer` bound to real
allocator memory, and the pipeline layout the renderer builds naming all of it), with and without
`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`:
- **The rung is `xud_descriptor`, above the allocator and below the renderer**, and it is **internal** like
  every layer below the public API: `xud.h` declares no descriptor type and no descriptor function, `xud_core.h`
  grows a typed `XudDescriptor*` field, and only `tests/` includes `xud_descriptor.h`. It is **context-owned,
  not renderer-owned**, and that is the load-bearing half of the ownership order: a `VkBuffer` is created from
  memory the allocator owns and is named by the pipeline layout the renderer builds, so the order in is
  **allocator → descriptor → renderer** and the order out is **renderer → descriptor → allocator**. `xud_renderer_open()`
  and `xud_pipeline_open()` both take the `const XudDescriptor*` and **borrow** it — the renderer draws against
  these sets, it does not own them.
- **Header facts** (grepped in `/usr/include/vulkan/vulkan_core.h`, 1.4.363, no driver call): the stamps are
  `VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO = 32`, `…_POOL_CREATE_INFO = 33`,
  `…_SET_ALLOCATE_INFO = 34`, `VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET = 35`, `VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO = 12`,
  all distinct; `VkDescriptorSetLayoutBinding` and `VkDescriptorPoolSize` are **not stamped** (no `sType`/`pNext`,
  like the `VkVertexInput*` rows the SoA decision records), so the "stamp the whole array" rule has nothing to
  stamp in them and only the create-infos carry a stamp. **Naming traps that bit the build here**: `VkDescriptorPoolSize`'s
  member is `descriptorCount`, **not** `maxDescriptorCount`; `VkWriteDescriptorSet`'s members are `pBufferInfo`/`pImageInfo`/`pTexelBufferView`,
  **not** `pDescriptorBufferInfo`/`pDescriptorImageInfo`; and `xud_descriptor_fill_set_allocate_info()`
  **writes the record's own row**, so it takes a non-`const` `XudDescriptor*` — the const form is a compile
  error (`assignment of member 'sType' in read-only object`), not a warning.
- **Two of this rung's rows return `void`** — `PFN_vkGetBufferMemoryRequirements` and `PFN_vkUpdateDescriptorSets`
  — so neither goes through `xud_result_from_vk()`: the requirements row is `memset` before the call and a
  `size` that came back 0 is a refusal (`XUD_VULKAN_ERROR`) rather than a success, and the write's inputs
  (set, buffer, range) are checked live **before** the call, because after it there is nothing to check.
  That is the zero-state-before-the-call / check-after discipline `vkDestroyDevice`/`vkGetDeviceQueue`/`vkFreeMemory` already follow.
- **No loader slot was added** — all nine names this rung calls (`vkCreateDescriptorSetLayout`,
  `vkDestroyDescriptorSetLayout`, `vkCreateDescriptorPool`, `vkDestroyDescriptorPool`,
  `vkAllocateDescriptorSets`, `vkUpdateDescriptorSets`, `vkCreateBuffer`, `vkDestroyBuffer`,
  `vkGetBufferMemoryRequirements`, `vkBindBufferMemory`) are rows of Volk's `VolkDeviceTable`, already
  filled in bulk by `xud_loader_bind_device()`, and every one is checked for NULL before its first call.
- **The caps gate is a refusal, not a shrink** (`xud_descriptor_caps_support()`): `maxBoundDescriptorSets`
  ≥ `XUD_DESCRIPTOR_SET_COUNT`, `maxPerStageDescriptorUniformBuffers`/`maxDescriptorSetUniformBuffers`
  ≥ 1, `maxPerStageDescriptorSamplers`/`maxDescriptorSetSamplers` ≥ `XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE`,
  `maxPushConstantsSize` ≥ `XUD_DESCRIPTOR_PUSH_SIZE` and `maxUniformBufferRange` ≥ `XUD_DESCRIPTOR_UNIFORM_BLOCK_SIZE`.
  A device that cannot hold this layout answers `XUD_UNSUPPORTED` — **a layout silently cut to fit is a layout
  the pipeline above would be created against and the shaders would then not match**, which is the same reason
  the swapchain step refuses rather than clamps. Every limit is read off the caps' recorded
  `VkPhysicalDeviceLimits`, never off the S5.4 note's numbers (that note's `maxPushConstantsSize` 128 and
  today's 256 already disagree, see the SoA decision).
- **`XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE 16u` is a layout ceiling, not a caps read**: growing it means recreating
  the set-1 layout, the pipeline layout and every pipeline that names it, which the repo's "explicit pipelines,
  created once at init" rule forbids at runtime — so it is a constant with a `static_assert` beside it, and the
  pool is sized for both descriptor types anyway (`XUD_DESCRIPTOR_POOL_SIZE_COUNT 2u`, one `VkDescriptorPoolSize`
  row per type) so the texture step allocates its set from the same pool without growing anything.
- **Set 1's layout is declared and its set is not allocated**, and that split is the whole point of the rung:
  `vkAllocateDescriptorSets` is asked with `descriptorSetCount = 1u` and `pSetLayouts = &setLayouts[XUD_DESCRIPTOR_SET_PER_FRAME]`,
  so `descriptor->sets[XUD_DESCRIPTOR_SET_MATERIAL]` stays `VK_NULL_HANDLE` and the GPU test asserts it. The
  texture step owns the samplers that set would hold, and `pImmutableSamplers` is NULL in both bindings for the
  same reason. The **partially-bound / descriptor-indexed** form the design sketch wants is a stated gate, not
  a row: `VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT` and `VkDescriptorSetLayoutBindingFlagsCreateInfo` are here
  (the struct sits under the `VK_EXT_descriptor_indexing` guard, `vulkan_core.h` 6771, stamp 1000161000) but
  nothing is chained into either layout's `pNext`, so every descriptor of set 1 has to be written before that
  set is bound — chaining it would also restate the physical layer's "the driver chained something we did not
  ask for" check, the same cost the dynamic-rendering gate was refused for.
- **A pipeline layout that names a push-constant range must name a filled one** (probed today, both with and
  without the layer): the validation layer answers `vkCreatePipelineLayout(): pCreateInfo->pPushConstantRanges[0].stageFlags is zero.`
  and `…size (0) is zero.` while the driver accepts both silently and hands back a live layout. That is why
  `xud_descriptor_fill_push_range()` **re-runs the caps gate** rather than trusting the bring-up below it —
  the pure half the headless run calls on its own must not be able to write a row the rung above would read.
- **The uniform block and the push row are spelled as float arrays plus scalars** (`XudFrameUniformRow` 120 B:
  `viewProj[16]` + `cameraPos[3]` + `time` + `fog[4]` + `ambient[4]` + `lightCount` + `reserved`; `XudPushRow` 80 B:
  `modelMatrix[16]` + `materialIndex` + `textureIndex` + `flags` + `reserved`) so std140 has no implicit holes to
  reason about and the sizes are `static_assert`ed against `XUD_DESCRIPTOR_PUSH_MIN_GUARANTEE 128u`, the spec's
  floor — the push row fits it with headroom, so the row is a fact about the layout, not about this driver.
- **Teardown is guard → pool → buffer → release → layouts**, and every step of that order is load-bearing:
  `xud_device_wait_idle()` first (a command buffer of the frame loop may still be reading this very buffer and
  this very set, the same boundary `xud_swapchain_close()`/`xud_allocator_close()` state), then `vkDestroyDescriptorPool`
  **before** anything else because there is **no `vkDestroyDescriptorSet`** (grep across `/usr/include/vulkan/`: zero hits)
  and the pool destroy is what retires set 0, then `vkDestroyBuffer`, then `xud_allocator_release()` on its range —
  buffer before release, because a `VkBuffer` bound to memory the allocator has just freed is a buffer naming
  freed memory — then the two layouts in reverse creation order. `xud_descriptor_close()` refuses to start when
  the guard answers `XUD_UNSUPPORTED` and records a `XUD_LOST` device while still tearing down, the rule
  `xud_device_close()`/`xud_swapchain_close()`/`xud_allocator_close()` follow, and `xud_descriptor_open()`'s
  failure path calls the same `xud_descriptor_retire()` before freeing the record, so a partial bring-up leaks nothing.
- **The GPU allocator test's numbers are now a delta, not a constant**, and that is a fact about the tree rather
  than about the allocator: the descriptor rung's uniform buffer is the first thing the allocator was ever asked
  for, so a context that comes back `XUD_OK` already owns **one block and one live range** (`context/allocator-one-block-now`
  replaced `context/allocator-no-block-yet`), the test's first fresh request lands **past** the descriptor's carve
  (`gpu-alloc/first-offset` is `> 0u`, not `== 0u`), the live count is `baseLive + 3u`, the reserved bytes include
  `baseBytes`, and the reuse-after-release check is `reuse.offset == small1.offset` — the coalesced free range,
  which is the point of the check and is still exactly what it tests.
- **Test counts after this step**: headless **2108 checks / 0 failures** (was 2020), `XUD_GPU_TESTS=1` **2588 / 0**
  (was 2460) and validation-clean with `VK_INSTANCE_LAYERS` unset **and** forced to `VK_LAYER_KHRONOS_validation`
  (the only `ERROR/validation` line in either run is still the diagnostics layer's own synthetic headless probe),
  `-DXUD_SANITIZERS=ON` reporting **only** the external allocations the S7/S8/S9/S10/S11/S12 blocks record — headless
  **72 B / 3 allocations**, byte-for-byte the S12 step's numbers, so the descriptor's pure half leaks nothing, and GPU
  **1352 B / 65** (was 1312 B / 63: one more libdecor 8 B and one more validation-layer 32 B whose four frames
  between `malloc` and `xud_device_open` are `<unknown module>`, i.e. inside `vkCreateDevice` and not inside lavvk
  and not inside xudlibc — the GPU descriptor test opens one more context), with `ASAN_OPTIONS=detect_leaks=0`
  answering 2108 / 2588 with 0 failures and no `runtime error` line; the clang build
  (`-DCMAKE_C_COMPILER=clang`) is warning-free at the same counts, `ar -t build/libxud.a` lists **39** objects
  with `xud_descriptor.c.o` among them, and `nm -u build/xud_test` still lists no `vk*` and no `glfw*` symbol
  while `ldd` lists no `libvulkan`, no `libglfw` and no `libGL`/`libEGL` — the descriptor rung calls no Vulkan
  entry point except through a row of Volk's device table.
- **Honest boundary after this step**: the layout exists, set 0 exists, the buffer is bound and the write binds
  it, and the pipeline layout the renderer created names all of it — but **no command binds it**: `xud_frame.c`
  calls neither `vkCmdBindDescriptorSets` nor `vkCmdPushConstants`, the built-in SPIR-V still reads no binding
  and takes no push constant, so the first triangle is still three vertices from `gl_VertexIndex`. What this rung
  bought is that the batch step's `vkCmdBindDescriptorSets` has a set to bind and the shapes step's push
  constants have a range to push into. Set 1's set, every `VkImage` and every sampler are the texture step's.

**Verified allocator facts (2026-10-25)** — what `src/xud/xud_allocator.h`/`xud_allocator.c` (S12) state, probed today
by a scratch program linked against `build/libxud.a` (loader → instance → physical → device, then deleted — the
probes are not committed and no library file was changed by them), by the headless test run (the pure half, with
synthetic caps and synthetic rows) and by the GPU test run (real `VkDeviceMemory` allocated, suballocated, grown,
released, reused, wrapped and freed), with and without `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`:
- **The rung is `xud_allocator`, above the swapchain and below the renderer**, and it is **internal** like every
  layer below the public API: `xud.h` declares no allocator type and no allocator function, `xud_core.h` grows a
  typed `XudAllocator*` field (the `void*` sentinel it had there is gone — this was the **last** one in `XudContext`),
  and only `tests/` includes `xud_allocator.h`. A range is not a caller's currency either: the rung above the
  allocator borrows `(memory, offset, size)` and names its own object with an `XudHandle`.
- **Header facts** (grepped in `/usr/include/vulkan/vulkan_core.h`, 1.4.363, no driver call): `VkMemoryAllocateInfo`
  3783 = `{ sType, pNext, allocationSize, memoryTypeIndex }` with stamp `VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO =
  5` (212) — one `VkDeviceMemory` per allocation and the type an **index into the caps' `memoryTypes[]`**, not a
  mask; `VkMemoryRequirements` 3790 = `{ size, alignment, memoryTypeBits }`; `VkMemoryType` 3521 =
  `{ propertyFlags, heapIndex }`; `PFN_vkAllocateMemory` 4551 returns `VkResult` and `PFN_vkFreeMemory` 4552
  returns **void**. **Naming trap, and it bit the test**: `VkMemoryHeap` 3516 = `{ size, flags }` — the heap's
  member is `flags`, the type's is `propertyFlags`, and writing `propertyFlags` on a heap row is a compile error,
  not a wrong value. `VK_MEMORY_TYPE` still greps to **zero hits** (a type is a property-flag/heap-index pair keyed
  by its index, so the caps row the S5.4 note labels "UNIFORM_BUFFER" is a label, not a constant); the heap
  constants are `VK_MEMORY_HEAP_DEVICE_LOCAL_BIT` 0x1 / `VK_MEMORY_HEAP_MULTI_INSTANCE_BIT` 0x2 /
  `VK_MEMORY_HEAP_TILE_MEMORY_BIT_QCOM` 0x8, and the property bit that matters here is
  `VK_MEMORY_PROPERTY_PROTECTED_BIT` 0x20.
- **The driver's memory rows today do not match the S5.4 note's numbers** — same reason the SoA decision note
  gives (the machine's Vulkan driver moved since 2026-10-18): **7 memory types / 1 heap**, `heap[0] size
  25005735936` (23.3 GB, not ~4 GB) `flags 0x1`, and the types' `propertyFlags` are `0x1` / `0x7` / `0xF` /
  **`0x21`** (the fourth carries the protected bit) / `0x1` / `0x7` / `0xF` — i.e. three distinct rows and four
  duplicates. Everything the allocator decides is read off the caps, never off that note, and the GPU test asserts
  the heap size it read rather than a constant.
- **The validation layer is a hard signal, not a suggestion, and the driver accepts almost every wrong shape here
  silently** (probed both ways): `allocationSize 0` answers `VK_SUCCESS` with a live handle on the driver while the
  layer answers `pAllocateInfo->allocationSize is 0` + `VUID-VkMemoryAllocateInfo-allocationSize-07897`/`-07899` +
  `VUID-vkAllocateMemory-pAllocateInfo-01714`; `memoryTypeIndex 99` answers `VK_ERROR_OUT_OF_RESOURCES` on the
  driver and `memoryTypeIndex (99) is not a valid index. Device only advertises 7 memory types` +
  `VUID-VkMemoryAllocateInfo-memoryTypeIndex-01872` with it; `allocationSize 25005735937` (one byte past the heap)
  answers `VK_SUCCESS` on the driver and `VUID-vkAllocateMemory-pAllocateInfo-01713` ("… is 25005735937 bytes from
  heap 0, but the size of that heap is only 25005735936 bytes") with it, while `25005735936` exactly answers
  `VK_SUCCESS` both ways; and `memoryTypeIndex 3` (the protected row) answers `VK_SUCCESS` on the driver and
  `pAllocateInfo->memoryTypeIndex (3) includes the VK_MEMORY_PROPERTY_PROTECTED_BIT memory property, but the
  protectedMemory feature is not enabled` with it. Every one of those is therefore a **library rule enforced
  before the call**, not a code the driver happens to answer.
- **`vkFreeMemory` on a handle the driver does not know is the same shape read from the other side** (probed):
  the layer answers `vkFreeMemory(): memory Invalid VkDeviceMemory Object 0x…` +
  `Couldn't find VkDeviceMemory Object 0x… This should not happen and may indicate a bug in the application` +
  `VUID-vkFreeMemory-memory-parameter`, twice, and the driver answers nothing. The allocator's own live-block
  bookkeeping is what prevents that: `xud_allocator_close()` frees each block's handle **once**, reads it into a
  local before clearing the row (the IN-parameter rule every destroy in this tree follows), and refuses a
  double-free by clearing the row first.
- **Protected memory is never a candidate, for a structural reason rather than a driver answer**: `protectedMemory`
  is `VkPhysicalDeviceProtectedMemoryFeatures` (5871), a **chained** feature struct — the same `pNext` shape the
  physical layer's "the driver chained something we did not ask for" check depends on, and the same shape AGENTS.md
  keeps as a future gate. This library chains nothing into the caps' features (probed: that row came back NULL) and
  enables nothing but its three wanted core features, so no device this tree creates has `protectedMemory` enabled.
  `xud_allocator_select_memory_type()` therefore takes **no** `VkPhysicalDeviceFeatures` at all and skips every type
  whose `propertyFlags` carry `VK_MEMORY_PROPERTY_PROTECTED_BIT` — the feature that needed a protected allocation
  is not available, so the allocation that needed it does not get a type.
- **The type rule is the spec's rule read as a search**: the type must be one the caps actually have (`index <
  memoryTypeCount`), its bit must be **set in `requirements->memoryTypeBits`**, its heap must exist
  (`heapIndex < memoryHeaps`) and the block being bound to it must fit that heap's `size`; among the candidates the
  **device-local** one wins (geometry and descriptor data live in device memory, which is the design sketch's point
  read as a fact), otherwise the lowest-index candidate. `memoryTypeBits == 0` is `XUD_UNSUPPORTED` — no type may
  serve this object, which is not a bad argument — and a request bigger than every candidate's heap is a refusal,
  not a smaller allocation.
- **The block policy is this layer's own ceiling, stated as constants and asserted** (the `xud_swapchain.h`
  pattern): `XUD_ALLOCATOR_BLOCK_MIN_SIZE` 64 KiB → `XUD_ALLOCATOR_BLOCK_MAX_SIZE` 16 MiB, doubling in between, so
  a request above the max is `XUD_OUT_OF_RESOURCES` — **a request bigger than one block is a request this layer
  cannot serve, not a request that gets a bigger block**, because a quietly-larger block is a block whose later
  carve sites are unreachable; `XUD_ALLOCATOR_RING_SIZE` 256 KiB is the transient region, `XUD_ALLOCATOR_SPLIT_MIN`
  64 B is the smallest hole worth recording, `XUD_ALLOCATOR_ALIGN_MAX` 256 is the largest alignment this layer will
  honour (the caps' `minAlignmentForExtendedData` and the `VkMemoryRequirements` alignment of every buffer/image
  this tree will ask for are at or below it), and the capacity doubling sequences (`XUD_ALLOCATOR_BLOCK_MIN_CAPACITY
  4` → `XUD_ALLOCATOR_BLOCK_MAX_CAPACITY 256`, `XUD_ALLOCATOR_RANGE_MIN_CAPACITY 16` → `XUD_ALLOCATOR_RANGE_MAX_CAPACITY
  4096`) carry the same two asserts the handle layer's do, so a sequence lands on its bound exactly instead of
  refusing a row one step early. Block sizes are `align_up(wanted, 256)` — the `VkMemoryType`/`VkMemoryHeap` pair is
  keyed by index, so alignment is this layer's rule, not a driver's.
- **The free list is one row per hole, coalesced on release**, and the generation bump is the stale-handle rule read
  at the one place a row goes free: `xud_allocator_release()` retires the live row (size → 0, generation + 1) and
  merges every touching row in the same block, so a range a caller kept across a release stops naming it **and**
  every range that named it before stops naming it. `xud_allocator_find_range()` is the pure half of the acquire —
  same-type filter, alignment arithmetic (`aligned = align_up(block.offset + row.offset, alignment)`), fits-check —
  and answers `XUD_ALLOCATOR_NO_RANGE` rather than a refusal, because "no hole fits" is a fact, not an error: the
  acquire above turns it into a new block. `xud_allocator_range_get()` reads a live row's own offset and size, so
  what a rung above holds is the row's truth, never its request.
- **The ring is one block, recorded in the block array**: `XudAllocator::ringBlockIndex` + `XUD_ALLOCATOR_NO_BLOCK`
  0xFFFFFFFF is the "no ring yet" state (a zero-initialised allocator reads as index 0, which would be a live block),
  and the first `xud_allocator_ring_acquire()` creates the ring's one `VkDeviceMemory` and records it as a block
  row, so `xud_allocator_close()` frees it with the rest rather than through a second path. The ring **bump-allocates
  and never frees**: `xud_allocator_ring_reset()` rewinds the cursor for the next frame, and a request larger than
  the ring is `XUD_OUT_OF_RESOURCES` rather than a wrap that overwrites live transient data. A wrap is a rewind to
  offset 0 in the same block, so a caller above the ring holds an offset per frame, never a range it must release.
- **Teardown is guard → free → free**: `xud_allocator_close()` runs `xud_device_wait_idle()` first, because a
  submission or a presentation may still be reading the very blocks being freed (the frame loop's `vkCmdDraw` and
  the `vkQueuePresentKHR` above submit through this device), and a guard that cannot run (`XUD_UNSUPPORTED`) is a
  teardown this rung must not start, while a **lost** device (`XUD_LOST`) is still freed and still reported — the
  same rule `xud_device_close()` and `xud_swapchain_close()` follow. An allocator that never allocated a block may
  also never have allocated its host arrays, so both `free()`s are guarded: `free(NULL)` is undefined, not a no-op.
- **Test counts after this step**: headless **2020 checks / 0 failures** (was 1916), `XUD_GPU_TESTS=1` **2460 / 0**
  (was 2284) and validation-clean with `VK_INSTANCE_LAYERS` unset **and** forced to `VK_LAYER_KHRONOS_validation`
  (the only `ERROR/validation` line in either run is still the diagnostics layer's own synthetic headless probe),
  `-DXUD_SANITIZERS=ON` reporting **only** the external allocations the S7/S8/S9/S10/S11 blocks record — headless
  **72 B / 3 allocations**, byte-for-byte the S11 step's numbers, so the allocator's pure half leaks nothing, and GPU
  **1312 B / 63** (was 1272 B / 61: one more libdecor 8 B and one more validation-layer 32 B whose four frames
  between `malloc` and `xud_device_open` are `<unknown module>`, i.e. inside `vkCreateDevice` and not inside lavvk
  and not inside xudlibc — the GPU allocator test opens one more context), with `ASAN_OPTIONS=detect_leaks=0`
  answering 2020 / 2460 with 0 failures and no `runtime error` line; the clang build
  (`-DCMAKE_C_COMPILER=clang`) is warning-free at the same counts (it needed one test fix: an all-zero
  `VkMemoryRequirements` passed as a `const VkMemoryRequirements*` is a
  `-Wuninitialized-const-pointer` error there, so the synthetic `ask` is `memset` like every other synthetic in
  this file), `ar -t build/libxud.a` lists **38** objects with `xud_allocator.c.o` among them, and `nm -u build/xud_test`
  still lists no `vk*` and no `glfw*` symbol while `ldd` lists no `libvulkan`, no `libglfw` and no `libGL`/`libEGL`
  — the allocator calls no Vulkan entry point except through a row of Volk's device table.
- **Honest boundary after this step**: `VkDeviceMemory` is allocated, suballocated, grown, released, reused, wrapped
  and freed — against real caps, with the validation layer clean — but **nothing above the allocator has asked it
  for anything**: there is still no `VkBuffer`, no `VkImage`, no vertex stream, no descriptor set and no batch, so a
  context that comes back `XUD_OK` owns an allocator with no block in it, no `XudMemoryRange` is held by any rung,
  and `xud.h` still declares no create/close function that takes an `XudHandle`. The batch / shapes / descriptor
  steps are what make these ranges mean anything. **Superseded 2026-10-25 (S13)**: the descriptor rung landed above
  it and carved the first range out of the first block, so an `XUD_OK` context's allocator owns one block and one
  live range — see *Verified descriptor-layout facts (2026-10-25)* for what is left of that sentence now.

**Verified first-triangle facts (2026-10-25)** — what `src/xud/xud_shader.h`/`.c`, `xud_pipeline.h`/`.c`,
`xud_renderpass.h`/`.c`, `xud_frame.h`/`.c` and `xud_renderer.h`/`.c` (S11) state, probed today by the
headless run (the pure halves: SPIR-V header check, create-info assembly, submit/present rows) and by
the GPU run in the debug config (a real pipeline, a real render pass, one real framebuffer + image view
per swapchain image, and **four real frames drawn** through `xud_renderer_draw()`, with and without
`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`):

- **The ladder is five rungs, and each owns exactly what it can destroy**: shader (one `VkShaderModule`)
  → pipeline (two modules + layout + pipeline, dying pipeline → layout → modules) → render pass +
  framebuffer array (one `VkRenderPass`, one `VkFramebuffer` + `VkImageView` per swapchain image) →
  frame loop (command pool + command buffer + acquire semaphore + one fence per image) → renderer (the
  owner of all four). Nothing above the device calls a Vulkan entry point except through a row of one
  of Volk's two tables, and **no loader slot was added** — the renderer's names are rows of
  `struct VolkDeviceTable`, already filled in bulk by `xud_loader_bind_device()`, which itself requires
  the `instanceTable.vkGetDeviceProcAddr` row it resolves them through. The plan's "`vkGetDeviceProcAddr`
  becomes a loader slot here" is therefore already satisfied by the Volk step; what this step adds is the
  **row check before every call** (a name whose extension is not enabled on the instance answers NULL).
- **The renderer's open order is render pass → pipeline → framebuffer array → frame, and the close order
  is the exact reverse**, and both halves are load-bearing. The pass comes FIRST because the core-1.4
  binding makes it an argument of the pipeline's create: `VUID-VkGraphicsPipelineCreateInfo-dynamicRendering-06576`
  says that with `dynamicRendering` not enabled (it is not — see the settled render-pass decision) a graphics
  pipeline with pre-rasterization or fragment state must name the pass it belongs to, and the validation layer
  here reports `VK_NULL_HANDLE` in that field as a hard error (probed today, both with and without the layer
  asked for). A framebuffer is created **FOR** a pass too (its attachment list must match the pass's descriptions
  exactly) and a frame records a draw naming a pipeline, a pass and a framebuffer row. On the way out the
  pipeline dies **before** the pass, because the pipeline is what names that pass's handle.
- **`pName` is `"main"`, never NULL** (`XUD_PIPELINE_ENTRY_POINT`): the validation layer refuses a NULL entry
  point name against both `pName` VUIDs here, and `glslangValidator` writes `main` into both SPIR-V modules,
  so the name is a fact about the built bytes, not a guess.
- **`VK_ATTACHMENT_LOAD_OP_NONE` (1000400000) and `VK_ATTACHMENT_STORE_OP_NONE` (1000301000) are extension-gated
  enum values**, not core ones: they need `VK_KHR_load_store_op_none`/`VK_EXT_load_store_op_none` (and the store
  one also `VK_KHR_dynamic_rendering` or `VK_QCOM_render_pass_store_ops`), none of which this library enables, so
  the validation layer refuses them as hard errors and the stencil ops are the **core** `LOAD`/`STORE` pair. The
  attachment has no stencil aspect in play at all — the row is colour-only — so the pair is a formality, not a
  cost, and it is the form the floor allows.
- **The attachment layout pattern is `COLOR_ATTACHMENT_OPTIMAL` in / `PRESENT_SRC_KHR` out**, and the subpass's
  colour **reference** uses the `initialLayout`, never the `finalLayout`: `VUID-VkAttachmentReference-layout-03077`
  forbids `PRESENT_SRC_KHR`, `UNDEFINED`, `PREINITIALIZED` and `ZERO_INITIALIZED_EXT` in an attachment reference.
  The pass is what transitions the swapchain image into the drawable layout and back out to the present layout.
- **`vkDestroyRenderPass`'s handle is an IN-parameter**, so `xud_renderpass_close()` saves it before clearing the
  field — clearing first would destroy `VK_NULL_HANDLE` and leave the pass alive, which the validation layer reports
  as `VUID-vkDestroyDevice-device-05137` ("VkDevice has N leaked objects that have not been destroyed") at the
  following `vkDestroyDevice`. Same shape as every other destroy in this tree, one rung higher.
- **`vkWaitSemaphores` is a core-1.2 entry point and this library's instance sits at the core-1.0 floor**
  (`XUD_INSTANCE_API_FLOOR == VK_API_VERSION_1_0`), so its device-table row is **nil** here and requiring it made
  `xud_frame_open()` answer `XUD_UNSUPPORTED` (probed: the row is NULL, every other row the frame needs is filled).
  The acquire gate is therefore the **core-1.0 form of the same boundary**: `VkSubmitInfo.pWaitSemaphores` +
  `pWaitDstStageMask` (vertex stage) on the queue, not a host-side wait. `xud_frame_fill_submit()` takes no
  `VkSemaphoreWaitInfo` at all, and `XudFrame` has no `waitInfo` field.
- **`passBegin.clearValueCount` is `XUD_RENDERPASS_ATTACHMENT_COUNT` (1), not 0**: `VUID-VkRenderPassBeginInfo-clearValueCount-00902`
  requires at least highest-index-cleared + 1 entries when a pass uses `VK_ATTACHMENT_LOAD_OP_CLEAR`, and the driver
  accepts 0 silently — the validation layer does not, and the triangle then draws over whatever the image held.
- **A swapchain image has no layout until something gives it one**, so the frame records one `VkImageLayout` per
  image (`XudFrame::imageLayouts`, opened at `VK_IMAGE_LAYOUT_UNDEFINED`, updated to the pass's `finalLayout` after
  the submit that ran the pass and never before — a barrier recorded into a command buffer is not a transition
  that happened yet, so a refused submit leaves the record at the layout the image really is still in, which is
  what keeps the NEXT frame's barrier's `oldLayout` a fact rather than a guess) and issues a `vkCmdPipelineBarrier`
  carrying one `VkImageMemoryBarrier` **before** `vkCmdBeginRenderPass` to transition it. Without that transition
  the validation layer refuses the submit: "command buffer ... expects VkImage ... to be in layout `COLOR_ATTACH
  MENT_OPTIMAL` — instead, current layout is `UNDEFINED`". Four facts about that barrier:
  the members are `oldLayout`/`newLayout` (**not** `srcLayout`/`dstLayout`, and there is no `flags` member on
  `VkImageMemoryBarrier`); `srcQueueFamilyIndex`/`dstQueueFamilyIndex` must both be set and equal (0 here) because an
  unset pair on a `VK_SHARING_MODE_EXCLUSIVE` image is read as garbage family indices; the stage masks must **support
  the colour-attachment access bits**, so `VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT` is used (`TOP_OF_PIPE` alone and
  `VERTEX|FRAGMENT` alone are both refused for `VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT` — probed); and the barrier is
  inside the command buffer, before the pass, so it needs no host-side sync of its own.
- **Depth test and depth write are `VK_FALSE` in the pipeline**, not `VK_TRUE`: the render pass has **no depth
  attachment** (that would need a `VkImage` + `VkDeviceMemory` of its own, i.e. the allocator rung that is not in the
  tree), and a depth test against no depth buffer is implementation-dependent rather than a deterministic first
  triangle. The depth attachment lands with the allocator step; this file says so rather than pretending the buffer exists.
- **`xud_framebuffer_fill_create_info()` takes the owning `XudFramebuffer`, not a bare `VkImageView`**, and writes
  `pAttachments = &framebuffer->view`: the create-info is read **after** the call returns, so the attachment row must
  point at storage that outlives the call, and the framebuffer's own `view` field is the only such storage in this layer.
- **The SPIR-V header check is the spec's rule, not the tool's**: word 0's fixed 24 bits plus a version byte in
  `0x00..0x0B`, because `spirv-val` here rejects every other low byte for **this** module (probed: 0x02, 0xff, 0x00 all
  answer "Invalid SPIR-V magic number") while the spec — and a driver — accept a 1.0 or 1.5 module too; a check that
  accepted only glslang's current magic would refuse valid modules. Word 4 (the instruction count) is deprecated,
  `spirv-val` accepts 5 written into it (probed), and nothing here checks it.
- **The shaders are built, never committed**: `shaders/solid.vert` + `shaders/solid.frag` → `glslangValidator -V` →
  `build/gen/solid_{vert,frag}.spv` → `spirv-val` **in the same rule** → `scripts/spv_to_header.cmake` →
  `build/gen/xud_solid_spv.h` of two `uint32_t` arrays (`xudSolidVertSpv` 354 words, `xudSolidFragSpv`), compiled into
  the library through a custom target that makes the generated header a **build dependency** of `xud` (the dependency
  scanner cannot see an include of a file that does not exist yet on a clean build). A missing `glslangValidator` or
  `spirv-val` is a `FATAL_ERROR`, the same rule `wayland-scanner`'s `find_program` uses. **CMake 4.4.4 quirks probed
  today**: it rejects `string(UPPERCASE)`/`string(LOWERCASE)`/`string(SPLIT)` outright and rejects `cmake -P`
  **positional** arguments (`ARGS`/`ARGV` both empty), so the array names and the include guard are passed in as one
  `-D` variable per input (`SPV_COUNT` + `SPV_ARRAY_<i>`/`SPV_INPUT_<i>`/`SPV_GUARD`/`SPV_OUT_HEADER`) — a list-valued
  `-D` inside `add_custom_command` survives `VERBATIM` as a literal `\;` and splits the shell line without it, verified
  both ways.
- **The out-param guard pattern is `(*out == NULL)` rejected, `(*out != NULL)` accepted** — an open function refuses a
  pointer that already owns the object. The inverted form (`*out == NULL` → refuse) is what made `xud_renderer_open()`
  answer `XUD_INVALID_ARG` for a freshly zero-initialised context field and `xud_init_context()` answer
  `XUD_INVALID_ARG` for valid extents; it was fixed in all six places (`xud_frame.c`, `xud_renderpass.c` ×3,
  `xud_pipeline.c`, `xud_renderer.c`) before the first GPU run.
- **The test scaffolding's three halves are all in place, and the validation half is now an in-run gate,
  not a grep**: the headless run stays green with no device present (the five new rungs' pure halves —
  SPIR-V header check, pipeline/renderpass/framebuffer create-info assembly, the submit and present
  rows — run there), the GPU run is opt-in via `XUD_GPU_TESTS=1`, and the validation-clean rule is read
  **in run**: the diagnostics layer's log callback counts every message it is handed into the struct it
  was given, and the GPU checks assert that the context's own `errorMessages` is still 0, so a validat
  ion complaint anywhere in a GPU run fails the run rather than going unnoticed (the shell grep is stil
  l run alongside, as a second opinion). `xud_test_gpu_draw()` adds the failure shapes of the frame loo
  p against **this** context's live objects rather than against synthetic ones — a NULL out-index, a NUL
  L pipeline, a NULL pass, a NULL framebuffer array, a zero row count, a row count that is not the swap
  chain's, a NULL renderer, and the renderer's own count-vs-array rule — and asserts after them that
  `framesDrawn` is still 0 and `lastFrameFence` is still `VK_NULL_HANDLE`, i.e. every refusal fired bef
  ore the acquire, which is the point of checking the rows and their owners first.
- **Test counts after this step**: headless **1916 checks / 0 failures** (was 1739), `XUD_GPU_TESTS=1` **2284 / 0**
  (was 2068) and validation-clean with `VK_INSTANCE_LAYERS` unset **and** forced to `VK_LAYER_KHRONOS_validation`
  (the only `ERROR/validation` line in either run is still the diagnostics layer's own synthetic headless probe),
  `-DXUD_SANITIZERS=ON` reporting **only** the external allocations the S7/S8/S9/S10 blocks record plus one more
  outside this tree — headless **72 B / 3 allocations** (byte-for-byte the S10 step's numbers) and GPU **1272 B / 61**
  (was 1136 B / 58: one more libdecor 8 B and one more validation-layer 32 B, because the GPU draw test opens one
  more context, and one 96 B allocation whose whole stack is `malloc ← wl_display_read_events ← wl_display_dispatch
  _queue_timeout ← wl_display_dispatch_queue ← wl_display_roundtrip_queue (all four frames inside
  `libwayland-client.so.0`) ← four frames inside the driver ← xud_swapchain_close`, i.e. the compositor traffic the
  present step generates, which nothing in xudlibc allocates, frees or can see), with
  `ASAN_OPTIONS=detect_leaks=0` answering 1916 / 2284 with 0 failures and **no** `runtime error` line; the clang
  build (`-DCMAKE_C_COMPILER=clang`) is warning-free over the new set and answers the same counts, `ar -t build/libxud.a`
  lists **37** objects with the five new `.c.o` among them, and `nm -u build/xud_test` still lists no `vk*` and no
  `glfw*` symbol while `ldd` lists no `libvulkan`, no `libglfw` and no `libGL`/`libEGL`.
- **Honest boundary after this step**: a triangle is drawn — four frames, acquired/recorded/submitted/presented through
  the renderer, with the image index moving and the per-image fence waited and reset — but it is **one hard-coded
  `vkCmdDraw` of three vertices generated from `gl_VertexIndex`**, with no vertex buffer, no push constant, no descriptor
  set, no camera and no batch above it, and no depth buffer beside it. `xud.h` still declares no draw call and no object
  handle, so nothing a caller can name is drawn yet: the shapes/batch/descriptor/allocator steps are what make this
  triangle mean anything.

**Verified handle layer facts (2026-10-24)** — what `src/xud/xud_handle.h`/`xud_handle.c` (S10) state, probed today
by the headless test run (the layer calls **no driver function at all** and includes **no Vulkan header**, so the
headless run covers it completely and the GPU run only adds the context-level checks) and by reading the pair:
- **The rung is `xud_handle`, above the swapchain and below the renderer**, and it is the storage rung for the
  object kinds a caller can name (textures, fonts, meshes, materials, models). A handle addresses an object that
  lives in GPU memory the allocator rung owns and that the renderer rung draws — **neither rung is in the tree yet**,
  which is why every slot's `record` field is still the NULL sentinel it opens with and why nothing here calls a
  driver function. The type itself is public (`xud.h` declares `XudHandle`, because it is the one thing a caller
  holds); the table and these functions are internal like every layer below the public API: `tests/` may include
  `xud_handle.h`, user code must not, and `xud.h` still declares no create/close function that takes a handle.
- **The validation order is the load-bearing half**, and it is the repo's "stale handles must be caught, not UB"
  rule made a property of this layer rather than of whoever reads a handle: expected-kind range → the kind's array
  exists → `index` below that array's high mark → `generation` equality → `kind` equality → live bit. The expected
  kind is the **entry point's** kind, not the handle's — that is what makes a cross-kind handle a caught error
  instead of a read of the wrong array, and it is why the tag is not redundant with the array's identity (a texture
  handle's `(index, generation)` pair can match a live model slot by coincidence). The live bit is checked **last**
  because `generation` and `kind` can both match a **free** slot for a handle a caller fabricated rather than one it
  kept across a release, so a decoded handle always names a live slot. Every failure answers `XUD_INVALID_ARG`.
- **`XUD_HANDLE_KIND_NONE = 0u` is the tag of a handle that names nothing**, never stored in a live slot, so a
  zero-initialised handle (what a caller holds when it ignores the create call's `XudResult`) is refused by the tag
  compare before any array lookup happens. Kind values are contiguous from `NONE` upward and
  `XUD_HANDLE_KIND_COUNT == XUD_HANDLE_KIND_MAX + 1u` is asserted, so a kind value is an index into the table's own
  array of arrays and forgetting to move `XUD_HANDLE_KIND_MAX` with a new kind is a compile error, not a slot array
  that silently has no row for it. `xud.h` asserts `sizeof(XudHandle) == 12u` beside the type (the layout contract);
  `xud_handle.h` asserts the contiguity half (the type is public, the array bound is this layer's).
- **The capacity bound is this layer's own ceiling**, not a provable one — nothing about Vulkan says how many
  textures or meshes an application may hold — so `XUD_HANDLE_SLOT_MIN_CAPACITY 16u` grows to `XUD_HANDLE_SLOT_MAX_CAPACITY
  4096u` by doubling, and a slot wanted at the bound answers `XUD_OUT_OF_RESOURCES` rather than a quiet cut, because a cut
  slot array is an array whose later slots are unreachable and whose handles would then address the wrong record. Growing
  past the bound is not an option either: an unbounded array is an unbounded host allocation, and the repo's rule is that
  a refusal is reported, not hidden. The doubling shape is the one the rest of this tree uses (`xud_instance.c`,
  `xud_physical.c`, `xud_swapchain.c`'s image query), and the two asserts (`MIN > NONE`, `MAX % MIN == 0`) are what make
  that sequence land on the bound exactly instead of stepping past it and refusing a slot one row too early.
- **Allocation is reuse-first, and the generation bump is the stale-handle rule read at the one place a slot goes
  free**: the first free slot below the high mark when there is one, the high mark itself when there is not (growing
  that kind's array when the high mark has reached the capacity). `xud_handle_free()` bumps the generation in the
  same step that marks the slot free, so the handle in hand stops naming it **and** every handle that named it before
  stops naming it. It validates against the handle's **own** tag, so a stale handle, a cross-kind handle and a
  fabricated one are all refused there rather than double-releasing a slot or bumping a generation twice. The tag and
  the record are left as they were, so a free slot still says what it held last — the live bit is what distinguishes
  it. The 2^32-reuse wrap is documented in `xud.h` rather than refused here (the repo's own wording for that case).
- **The empty-table state is per kind, not per table**: `XudHandleTable` is one array per kind indexed by the kind
  value itself, so row 0 (`NONE`) exists and stays NULL forever, and an array is NULL until that kind's first
  allocation — the "no object of that kind was ever created in this context" state, which is what keeps a context
  that draws only meshes from paying for a font array. `xud_handle_table_live_count()` answers that state as 0, not
  as an error. `xud_handle_table_close()` frees every array that exists, clears the pointers and counts **before**
  freeing (a table torn down twice cannot double-free) and sets the out-pointer to NULL; it does not walk the slots,
  because there is nothing in a slot to close yet.
- **The table is owned per context, opened last and closed first**: `xud_init_context()` opens it after the swapchain
  (no caller-addressable object can exist before the context that owns it does) and `xud_close_context()` closes it
  before the swapchain, because the handle table is the **top** of the tree — a live table can name objects the rungs
  below it are about to retire, so it must die before them, one rung above the swapchain in the same direction a
  `VkSwapchainKHR` dies before the `VkDevice` it was created from. Two contexts have two tables, which is what makes
  a handle from one refused by the other rather than indexing the wrong array; `xud_handle_table_open()` refuses a
  pointer that already owns a table (`XUD_INVALID_ARG`), since two tables for one context would be two owners of the
  same slots. `xud_core.h`'s `allocator` field stays the one `void*` sentinel left. **Superseded 2026-10-25 (S12)**:
  the allocator rung landed and that field is typed now — see *Verified allocator facts (2026-10-25)*.
- **Test counts after this step**: headless **1739 checks / 0 failures** (was 1642), `XUD_GPU_TESTS=1` **2068 / 0**
  (was 1969) and validation-clean with `VK_INSTANCE_LAYERS` unset **and** forced to `VK_LAYER_KHRONOS_validation` (the
  only `ERROR/validation` line in either run is still the diagnostics layer's own synthetic headless probe), `-DXUD_SANITIZERS=ON`
  reporting **only** the external allocations the S7/S8/S9 blocks record — headless **72 B / 3 allocations** and GPU
  **1136 B / 58**, byte-for-byte the S9 step's numbers, so the handle layer leaks nothing (note for re-running: with
  leak detection on, LSan's exit path discards the test binary's buffered stdout, so the check counts have to be read
  with `ASAN_OPTIONS=detect_leaks=0`, which answers 1739 / 2068 with 0 failures); the clang build
  (`-DCMAKE_C_COMPILER=clang`) is warning-free and answers the same counts, `ar -t build/libxud.a` lists **32** objects
  with `xud_handle.c.o` among them, and `nm -u build/xud_test` still lists no `vk*` and no `glfw*` symbol while `ldd`
  lists no `libvulkan`, no `libglfw` and no `libGL`/`libEGL` — the handle layer includes no Vulkan header and calls no
  Vulkan entry point at all, so it adds nothing to the link.
- **Honest boundary**: the handle currency and the per-kind slot arrays are in the archive and tested, but a slot's
  `record` field is the NULL sentinel of a rung that does not exist — there is still no allocator, no render pass,
  no framebuffer and no batch, so **no handle names an object yet**, and `xud.h` still declares no function that takes
  one. That is the *Now* item: the first triangle end to end on the explicit `VkRenderPass` + `VkFramebuffer` path.
  **Superseded 2026-10-25 (S11)**: the render pass, the framebuffer per swapchain image, the pipeline, the shader
  modules and the frame loop are in the tree and a triangle is drawn (see *Verified first-triangle facts*); what is
  left of that sentence is the **allocator** (the one `void*` sentinel still in `xud_core.h`) and the **batch**, so
  a slot's `record` field is still the NULL sentinel and `xud.h` still declares no function that takes a handle.
  **Superseded 2026-10-25 (S12)**: the allocator rung landed and that field is typed now — what is left of that
  sentence is the **batch** and the shapes/descriptor steps above it, so a slot's `record` is still the NULL
  sentinel and `xud.h` still declares no function that takes a handle (see *Verified allocator facts (2026-10-25)*).

**Verified swapchain facts (2026-10-24)** — what `src/xud/xud_swapchain.h`/`xud_swapchain.c` (S9) state, probed today
by three scratch programs linked against `build/libxud.a` (loader → instance → physical → device → surface, then
deleted — the probes are not committed and no library file was changed by them) and by the GPU test run (a real
swapchain created, recreated at a second extent, and destroyed, with and without the validation layer); **no
allocator and no render pass exist in the tree yet**:
- **The rung is `xud_swapchain`, above the device and below the renderer**, and it is **internal** like every layer
  below the public API: `xud.h` declares no swapchain type and no swapchain function, `xud_core.h` grows a typed
  `XudSwapchain*` field (the `void*` sentinel it had there is gone), and only `tests/` includes `xud_swapchain.h`.
  `XudSwapchain` owns the one Vulkan object of this rung — a `VkSwapchainKHR` plus the `VkImage*` array
  `vkGetSwapchainImagesKHR` handed it — and borrows the `VkSurfaceKHR` (the window owns it), the `VkPhysicalDevice`
  (the physical layer owns it) and the `VkDevice` (the device layer owns it).
- **The three surface queries are instance-table rows, the swapchain names are device-table rows** — the same level rule
  every layer above already follows: `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` / `…FormatsKHR` / `…PresentModesKHR`
  take a `VkPhysicalDevice`, so they are rows of `struct VolkInstanceTable`, and `vkCreateSwapchainKHR` / `vkDestroySwapchainKHR`
  / `vkGetSwapchainImagesKHR` take a `VkDevice`, so they are rows of `struct VolkDeviceTable`. `xud_swapchain_open()` therefore
  requires **both** bound-handle rules before any call: `xud_loader_instance_bound(loader, window->surfaceInstance)` (the
  surface queries are rows of the instance the SURFACE was created for — `xud_window.c` records that instance for exactly
  this reason) and `xud_loader_device_bound(loader, device->device)`.
- **`vkDestroySwapchainKHR` is checked before `vkCreateSwapchainKHR`**, the same rule `xud_window.c` applies to a surface:
  a swapchain this layer cannot destroy is a swapchain it must not create.
- **The surface's answer here** (`vkGetPhysicalDeviceSurfaceCapabilitiesKHR` through the validation layer, probed today):
  `minImageCount 4`, **`maxImageCount 0`** (0 is the "no upper limit" answer — asking for 100 images answered `VK_SUCCESS`
  without the layer and `VUID-VkSwapchainCreateInfoKHR-minImageCount-01384` with it), `currentExtent 4294967295×4294967295`
  (the "undefined" answer — the compositor has no extent for a surface that was never mapped), `minImageExtent 1×1`,
  `maxImageExtent 16384×16384`, `maxImageArrayLayers 1`, `supportedTransforms`/`currentTransform 0x1` (IDENTITY only),
  `supportedCompositeAlpha 0x3` (OPAQUE|PRE_MULTIPLIED), `supportedUsageFlags 0x8009f` (COLOR_ATTACHMENT among them).
  `vkGetPhysicalDeviceSurfaceFormatsKHR` answers **16** rows (eight at `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR` = 0 and eight at
  `VK_COLOR_SPACE_PASS_THROUGH_EXT` = 1000104013, the same eight formats twice: `R16G16B16A16_SFLOAT`, `A2R10G10B10_UNORM_PACK32`,
  `A2B10G10R10_UNORM_PACK32`, `B8G8R8A8_SRGB`, `B8G8R8A8_UNORM`, `R8G8B8A8_SRGB`, `R8G8B8A8_UNORM`, `R5G6B5_UNORM_PACK16`),
  and `vkGetPhysicalDeviceSurfacePresentModesKHR` answers **2** modes: MAILBOX (1) and FIFO (2).
- **`VK_COLOR_SPACE_PASS_THROUGH_EXT` rows are unreachable unless the instance enables `VK_EXT_swapchain_colorspace`** —
  and it does not. The name **is** one of the 26 advertised at instance level (probed), but enabling it is an extension
  decision, not a default, so `xud_swapchain_select_format()` searches the advertised list for a **`(format, colorSpace)` pair**
  and only ever picks an SRGB_NONLINEAR row when one exists; the last-resort branch (first advertised pair) is for a surface
  that has no SRGB_NONLINEAR row at all, which the spec allows.
- **The present-mode enum is not contiguous** (0,1,2,3 then 1000111000/1000111001/1000361000), so the selection rule
  searches **by value**, never by position — an index into that list is nothing at all and the mode itself is the value
  the create-info carries. MAILBOX is preferred, FIFO is the only mode the spec guarantees, and a surface advertising only
  IMMEDIATE gets IMMEDIATE rather than a refusal, because IMMEDIATE is a present mode, not an absence of one.
- **`VkSwapchainCreateInfoKHR` is at `vulkan_core.h` 9095** (not near the other create-infos — the `KHR` create-infos sit
  in their own block), stamp `VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR = 1000001000`, and **has no `pQueueFamilyPriorities`
  member** (grep: zero hits — writing one is a compile error, `did you mean 'pQueueFamilyIndices'?`). `queueFamilyIndexCount 0`
  with `pQueueFamilyIndices NULL` is the spec's "application does not care which queue families" form, and it is what this
  layer writes: the physical layer already chose one family and the device layer already created a queue from it.
- **`VK_PRESENTATION_NONE` does not exist** (grep `VK_PRESENTATION_` across `/usr/include/vulkan/`: zero hits) and there is
  **no `VK_IMAGE_USAGE_PRESENT_BIT`** either — presentation is not a usage flag, so `imageUsage` is the one bit this library's
  one use of a swapchain requires, `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT`, intersected with `supportedUsageFlags` rather than
  asserted, and `imageSharingMode` is `VK_SHARING_MODE_EXCLUSIVE`.
- **`clipped = VK_TRUE`, not a comparison against `currentExtent`**: the surface here reports no current extent at all, so a
  comparison would be a comparison against nothing, and the images are clipped to the extent this layer asked for either way.
- **The validation layer catches the shapes this layer must not write** (probed with and without it — the driver accepts
  almost all of them silently): `minImageCount` below `caps.minImageCount` answers `VUID-VkSwapchainCreateInfoKHR-presentMode-02839`,
  an extent outside `minImageExtent`/`maxImageExtent` answers `VUID-VkSwapchainCreateInfoKHR-pNext-07781` and `VK_ERROR_OUT_OF_HOST_MEMORY`,
  an unsupported `(imageFormat, imageUsage, imageArrayLayers, imageExtent)` combination answers `VUID-VkSwapchainCreateInfoKHR-imageFormat-01778`,
  and a zero `imageUsage` answers `VUID-VkSwapchainCreateInfoKHR-imageUsage-parameter`. Without the layer, `minImageCount 100`,
  duplicate queue-family indices, `presentMode` not advertised, `compositeAlpha` not advertised, `preTransform` not advertised,
  `imageArrayLayers 2` and `flags = VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR` all answered `VK_SUCCESS` with a live swapchain.
- **`oldSwapchain` retires the old swapchain's images as part of the create, but the old handle still has to be destroyed** —
  the spec's own text. `xud_swapchain_recreate()` passes `oldSwapchain`, then calls `vkDestroySwapchainKHR` on the old handle
  explicitly before anything else about the new swapchain happens, because a driver that only retired it implicitly would
  leak it. A **failed** create does not retire it (probed: a create outside this surface's `maxImageExtent` answered
  `VK_ERROR_OUT_OF_HOST_MEMORY` with the old swapchain still usable), so the handle in hand stays alive and unchanged on
  every path that does not destroy it. A successful create followed by a **failed image query** destroys the new swapchain
  too and leaves the struct in the no-swapchain state with its record still at the extent it *had* a swapchain for — a record
  left at the NEW extent would make the retry above answer `XUD_ALREADY` with nothing in hand.
- **The image array is freed, never destroyed**: there is no `vkDestroyImage` call anywhere in this layer. The images belong
  to the swapchain and `vkDestroySwapchainKHR` retires them; `vkDestroyImage` on one of them is a double-destroy. `vkDestroyImageView`
  is not called here either — no image view of a swapchain image exists in the tree yet, that is the renderer rung's job.
- **`xud_swapchain_close()` calls `xud_device_wait_idle()` before the destroy**, and that guard is not decoration: a
  presentation may still be in flight against these very images (`vkQueuePresentKHR` is the renderer rung's call and it
  submits through the device above), so destroying the swapchain under it is destroying images a submission is still reading.
  A guard that cannot run (`XUD_UNSUPPORTED`) is a teardown this rung must not start; a **lost** device (`XUD_LOST`) is still
  destroyed and still reported, the same rule `xud_device_close()` follows.
- **`xud_swapchain_images_get()` is the accessor that makes a caller above hold an index rather than a `VkImage`** — a
  recreate replaces the whole array, so a handle kept across one is refused, not dangling. It is pure (no driver call),
  refuses `index >= imageCount` with `XUD_INVALID_ARG` and a struct whose handle is cleared with `XUD_UNSUPPORTED`: the
  repo's opaque-handle rule (`xud.h`) read at the one array this layer owns.
- **The image query is the one list here that CAN grow** — `xud_swapchain_query_images()` uses the doubling-capacity shape
  `xud_instance.c` and `xud_physical.c` use for the extension and queue-family lists (16 → 512), because `pCount` is input/output
  and there is no count-only form, and the array it grows is a heap member of its own struct. The two surface list queries,
  by contrast, are asked with the capacity of the fixed arrays of the struct being filled and refuse `VK_INCOMPLETE` with
  `XUD_OUT_OF_RESOURCES` — different bounds, different reasons, both written down where they can be read.
- **`xud_core.c`'s bring-up now completes through the swapchain and `xud_close_context()`'s one teardown order starts with
  it**: `… → device → surface → swapchain` on the way in, `swapchain → device → surface → window → physical → instance → diagnostics → loader`
  on the way out. The swapchain slots **before** the device because a `VkSwapchainKHR` is created from a `VkDevice`, so it
  must die before it — the same reason a `VkDevice` dies before its `VkPhysicalDevice`. `xud_init_context()`'s failure path
  calls `xud_close_context()`, so a bring-up that fails at the swapchain unwinds through the one order and leaks nothing.
- **Test counts after this step**: headless **1642 checks / 0 failures** (was 1529), `XUD_GPU_TESTS=1` **1969 / 0** (was 1786)
  and validation-clean with `VK_INSTANCE_LAYERS` unset **and** forced to `VK_LAYER_KHRONOS_validation` (the only `ERROR/validation`
  line in either run is still the diagnostics layer's own synthetic headless probe), `-DXUD_SANITIZERS=ON` reporting **only**
  the external allocations the S7 and S8 blocks record (headless debug-on 72 B / 3 allocations, GPU debug-on 1136 B / 58,
  headless `-DXUD_DEBUG=OFF` 8 B / 1, GPU `-DXUD_DEBUG=OFF` 240 B / 30 — each of those GPU counts exactly one more than
  the S8 step's because the GPU swapchain test opens one more window, and the debug-off GPU run's 30 allocations are 30
  × 8 B all of them the same `malloc ← libc ← g_malloc ← g_strdup ← gdk_set_allowed_backends ← libdecor-gtk.so ← libdecor_new
  ← _glfwInitWayland ← glfwInit ← xud_window_open` stack, while the debug-on run's extra 28 are 28 × 32 B whose four
  frames between `malloc` and `xud_device_open` are `<unknown module>`, i.e. inside `vkCreateDevice` and not inside lavvk
  and not inside xudlibc — nothing in xudlibc allocates, frees or can see either, and **no suppression is installed** to make the runs look clean),
  the clang build (`-DCMAKE_C_COMPILER=clang`) warning-free and same counts, `ar -t build/libxud.a` lists **31** objects with
  `xud_swapchain.c.o` among them, and `nm -u build/xud_test` still lists no `vk*` and no `glfw*` symbol while `ldd` lists no
  `libvulkan`, no `libglfw` and no `libGL`/`libEGL` — the swapchain layer calls no Vulkan entry point except through a row of
  one of the loader's two Volk tables.
- **Honest boundary after this step**: the swapchain is created, recreated at a second extent and destroyed, and the context
  owns it — but **nothing is drawn into its images yet**: no command buffer, no render pass, no framebuffer, no acquire/present
  pair, and the `VkFramebuffer` array the design sketch speaks of does not exist yet, so "the framebuffer array is recreated
  with the swapchain" is a rule the *Next* step writes against, not a rule this step could test. `xud_core.h`'s `allocator`
  field is the one rung still not in the tree, and it stays a `void*` sentinel for exactly that reason.

**Verified context ownership facts (2026-10-24)** — what `src/xud/xud_core.h`/`xud_core.c` (S8) state, probed today by the headless and the GPU test run (a real window, a real `VkDevice`, a real surface, created and destroyed inside one context) and by reading the layers above it; **no swapchain and no allocator exist in the tree yet** (the swapchain landed with the step above, S9, and `xud_core.h`'s field is typed now — the `allocator` is the one that remains):
- **The bring-up order is not the layer order bottom-to-top, it is the order the dependencies force**, and it is written in the header comment where it can be read: loader → diagnostics → **window** → GLFW's required instance extensions → instance (carrying them) → messenger → physical (through THAT instance) → GLFW's presentation check → device → surface. The window comes **before** the instance because `glfwCreateWindowSurface` resolves `vkCreateWaylandSurfaceKHR` through `vkGetInstanceProcAddr` **against the instance it is handed** (the S7 fact, one rung up), so GLFW's names have to be known before the instance exists. `xud_init_context()` reads them off the window with `xud_window_required_instance_extensions()` and hands them to `xud_instance_create()` as its `extraNames`/`extraCount` pair — which is the whole reason that parameter exists.
- **A merged instance enables four names here, not five**: GLFW's Wayland backend asks for exactly two (`VK_KHR_surface`, `VK_KHR_wayland_surface`), `VK_KHR_surface` is already one of the instance layer's three wanted names, and the instance layer's "enable nothing that was not advertised" rule plus the dedup does the rest. The GPU test asserts the dedup directly (`VK_KHR_surface` appears **exactly once** in the instance's enabled list) and that the wayland name is present — the load-bearing half, since a surface cannot exist without it.
- **`xud_close_context()` is the one teardown order, and it is the reverse of the creation order**: `device → surface → window → physical → instance → diagnostics → loader` (the swapchain, when it lands, slots **before** the device). Each step is load-bearing, and the header says which: a `VkDevice` is created from a `VkPhysicalDevice`, so it goes first; `vkDestroySurfaceKHR` needs the owning `VkInstance` alive and `xud_window_close()` **refuses** a window whose surface is still live, so the surface goes before the window; a `VkPhysicalDevice` is a query result **of** an instance, so it goes before it; and `xud_instance_destroy()` tears the debug messenger down through the still-alive instance **before** `xud_diagnostics_close()` frees the struct. The GPU test asserts the whole order completes with nothing refused.
- **`xud_init_context()`'s failure path calls `xud_close_context()`**, so a partial bring-up leaks nothing and there is exactly one teardown order in the tree rather than two. `xud_close_context()` is idempotent per rung (`XUD_ALREADY` is not a refusal) and returns the **first** non-`XUD_OK`, non-`XUD_ALREADY` result it sees — a lost device (`XUD_LOST`) and a NULL loader row that makes a teardown rung unsupported are **reported**, not hidden, and the remaining rungs still run because skipping one would leak an object the driver still holds.
- **The presentation check is a refusal, not a bad argument**: GLFW saying the chosen physical device cannot present to this window's system answers `XUD_UNSUPPORTED` part-way through the ladder, and the ladder then unwinds through the one teardown order. It is asked about the physical layer's **chosen** family (`caps.queueFamilyIndex`), which is the present half of the one-family rule the physical layer's graphics check is the other half of.
- **No queue fields in `XudContext`.** `xud_core.h` used to carry `void *queueGraphics` / `void *queuePresent` sentinels; they are gone, not filled. `XudQueue` is a **record** of a queue (see the S6 block: there is no `vkDestroyQueue`), both records live inside `XudDevice`, and the public API has no way to name a queue — so a context field for them would be a second, unowned copy of state the device already owns. `xud_core.h` says this rather than letting the reader infer it from an absent field.
- **The surface is not a context field, in any form.** It lives in `XudWindow` (`surface` + the borrowed `surfaceInstance` it was created for), and the context owns the window. `xud_core.h`'s teardown comment names the surface step so the reader knows where it goes, and the GPU test asserts `window->surfaceInstance == instance->instance`, i.e. the surface names the context's **own** instance, which is the handle `vkDestroySurfaceKHR` needs alive.
- **`XUD_VALIDATE` is snapshotted into the context, not re-read.** `xud_init_context()` copies the build switch into `context->validate` and opens the diagnostics struct on that copy, so `xud_close_context()` frees the struct it actually opened rather than one a rebuilt switch would imply. The GPU test asserts the snapshot and that a validation-on context has a diagnostics struct whose **error counter is still zero** — the validation-clean rule read as a check, not only as a log grep.
- **Two test-count facts changed with this step, and both are about what the headless run now does.** The headless run **creates real windows now** (a completed context is what `xud_init_context()` answers on a machine that has a loader and a window system), so `xud_test_no_leak()` no longer runs 1000 full cycles — it runs 1000 **rejected** extents, which is the leak-free argument it was ever making, and the 1000 full cycles moved to the GPU gate as `xud_test_gpu_context()`'s 25 create+destroy cycles (a real window and a real `VkDevice` each). The old `init/*` expectations that asserted `XUD_UNSUPPORTED` for valid extents are gone: valid extents now assert **either** outcome, and a completion must close back to nothing while a refusal must hand back nothing.
- **What the sanitizer runs report, and where none of it is ours** (probed today, `-DXUD_SANITIZERS=ON`, both the debug config and a `-DXUD_DEBUG=OFF` one): every allocation in every report has a whole stack outside this tree. The 8-byte one is the libdecor/libgdk leak the S7 block records (`malloc ← g_malloc ← g_strdup ← gdk_set_allowed_backends ← libdecor's GTK plugin ← libdecor_new ← _glfwInitWayland ← glfwInit`) — GLFW `dlopen`s libdecor when it is present, and since this step the leak appears in the **headless** run too, because the headless run creates windows now. The debug config adds **32 bytes per `vkCreateDevice`** whose stack is `malloc ← <unknown module> ×4 ← xud_device_open at the vkCreateDevice call line` — i.e. inside the **validation layer**, not inside lavvk and not inside xudlibc: a `-DXUD_DEBUG=OFF` build (which asks for no layer at all) reports **only** the libdecor leak, headless and GPU alike. Nothing in xudlibc allocates, frees or can see either one, so there is nothing to fix and **no suppression is installed** to make the runs look clean.
- Test counts after this step: headless **1529 checks / 0 failures** (was 525), `XUD_GPU_TESTS=1` **1786 / 0** (was 719) and validation-clean with `VK_INSTANCE_LAYERS` unset **and** forced to `VK_LAYER_KHRONOS_validation` (the only `ERROR/validation` line in either run is still the diagnostics layer's own synthetic headless probe), `-DXUD_SANITIZERS=ON` reporting **only** the external allocations above (headless 72 B / 3 allocations, GPU 1128 B / 57 in the debug config; 8 B / 1 headless and 232 B / 29 GPU in a `-DXUD_DEBUG=OFF` config, all libdecor), the clang build (`-DCMAKE_C_COMPILER=clang`) warning-free and same counts, `ar -t build/libxud.a` still lists **30** objects (this step added no module — `xud_core.c` was already in the archive), and `nm -u build/xud_test` still lists no `vk*` and no `glfw*` symbol while `ldd` lists no `libvulkan`, no `libglfw` and no `libGL`/`libEGL`.
- **Honest boundary after this step**: the context is wired to every layer that exists — loader, diagnostics, window, instance, physical, device, and the surface inside the window — and the whole lifecycle is in the archive, tested against the driver, and leak-free apart from the external allocations above. What is still open is everything **below the surface**: the swapchain (recreate on resize, per the design sketch), the renderer, and the GPU-memory allocator. `xud_core.h`'s `swapchain` and `allocator` fields stay `void*` sentinels for exactly that reason, and the GPU test asserts they are NULL rather than assuming.

**Verified loader bring-up (2026-10-16, the hand-rolled table — superseded by Volk on 2026-10-23, kept because the driver facts still hold)** — the shape the replaced `src/xud/xud_loader.c` used:
- `dlopen("libvulkan.so.1", RTLD_NOW|RTLD_GLOBAL)` succeeds (resolves to `libvulkan.so.1.4.363`); `dlsym(handle, "vkGetInstanceProcAddr")` resolves; the seed then resolves `vkEnumerateInstanceLayerProperties` (**3 layers advertised** here) and `vkEnumerateInstanceExtensionProperties`. `dlopen`/`dlsym`/`dlclose` come from **libc** — there is no `-ldl` and no `-lvulkan` anywhere: `ldd build/xud_test` lists neither, `nm -u build/xud_test` lists no `vk*` symbol.
- The PFN typedefs (`PFN_vkGetInstanceProcAddr`, `PFN_vkEnumerateInstanceLayerProperties`, …) **are** in `/usr/include/vulkan/vulkan_core.h` (guarded by `VK_API_VERSION_1_0`), so the loader's table is typed with them and the header includes `<vulkan/vulkan.h>` for `VK_API_VERSION_1_0` / `VK_NULL_HANDLE` only — headers are compile-time, they create no link dependency. `VK_NULL_HANDLE` is `0U` here, so it passes cleanly into the `VkInstance` parameter of a resolve call.
- **Strict-mode cast rule**: casting the `void*` that `dlsym` returns to a function-pointer type (or a function pointer back to `void*`) is a hard error under `-std=c11 -Wall -Wextra -Wpedantic -Werror` (`ISO C forbids conversion of object pointer to function pointer type`, and `the address of 'vkX' will always evaluate as 'true'`). `xud_loader.c` copies the bits instead: a `XudPfnBits { void *object; void (*function)(void); }` union + `memcpy` into the slot — verified to compile with gcc and clang under the project flags.
- **`static_assert` rule**: it needs `<assert.h>`, and the **message-less form is rejected by clang 22** under `-std=c11 -Wall -Wextra -Wpedantic -Werror` (`'_Static_assert' with no message is a C23 extension`). Always write `static_assert(cond, "why").`
- **One slot per name the layers above need, count kept exact (grown with the physical layer, 2026-10-18)**: `XUD_LOADER_SLOT_COUNT` is the field count of `XudLoader` — **20** today, of which `XUD_LOADER_GLOBAL_SLOT_COUNT` 3 are global-level and filled at loader bring-up, the rest instance/device/swapchain-level and filled by the layer that needs them through `xud_loader_bind`. The physical layer needed **two** new slots in all: `PFN_vkGetPhysicalDeviceProperties2` (S5.1, table 18 → 19) and `PFN_vkGetPhysicalDeviceMemoryProperties2` (S5.4, the driver half, table 19 → **20**); the other five names it needs were already instance-level slots. **Probed here** (dlopen only, no link dependency, `VK_INSTANCE_LAYERS` unset): `vkGetInstanceProcAddr` answers non-NULL for all **seven** physical-layer names — `vkEnumeratePhysicalDevices`, `vkGetPhysicalDeviceProperties`, `vkGetPhysicalDeviceProperties2`, `vkGetPhysicalDeviceFeatures2`, `vkGetPhysicalDeviceMemoryProperties2`, `vkGetPhysicalDeviceQueueFamilyProperties2`, `vkEnumerateDeviceExtensionProperties` — through a **bare** instance (`apiVersion = VK_API_VERSION_1_0`, no extension, no layer) and through the instance `xud_instance.c` creates, and answers NULL for all seven with `VK_NULL_HANDLE` in hand, which is why they are instance-level slots and not global ones. lavvk answers the `*2` forms even when `VK_KHR_get_physical_device_properties2` is **not** enabled on the instance in hand, so the slot is filled either way here; a driver that gates them answers NULL instead, and the layer that binds such a slot reports the nil slot rather than calling through it (the same check `xud_instance.c` applies to `createInstance`). Unlike the messenger pair these are **cached**: they name the driver's own code, not a layer instantiated per instance, so a slot stays valid across an instance recreation. The **four** device-layer names (`vkCreateDevice`, `vkDestroyDevice`, `vkGetDeviceQueue`, `vkDeviceWaitIdle`) needed **no** new slot — all four were in the table since S2 — and follow the same rule: probed 2026-10-18 through `xud_loader_resolve`, all four answer `XUD_OK` through a live `VkInstance` and `XUD_UNSUPPORTED` with `VK_NULL_HANDLE` in hand, so they are instance-level for resolution and `xud_device_open()` binds them before its first call (teardown then calls through the cached slot rather than resolving again). `vkGetDeviceProcAddr` is the one device-level name **not** slotted and not called anywhere in this tree yet: it needs the `VkDevice` itself, not the instance, and belongs to the renderer layer.
- **Handle-level rule (added with the instance layer)**: `xud_loader_resolve` takes the level-selecting handle as `const void *`, not `VkInstance` — the layers above have to pass a `VkPhysicalDevice`/`VkDevice`, and that would be an incompatible-pointer argument. The one cast back to `VkInstance` inside the call is sound only while every handle is a pointer, which is asserted: `static_assert(VK_USE_64_BIT_PTR_DEFINES == 1, ...)` holds with these headers. `xud_loader_bind(loader, handle, name, &loader->vkX)` is the public resolve-and-store helper (`resolvedCount` bumps only on empty→filled, so it is a property of the table, not of the calls) and `xud_loader_is_open(loader)` is the invariant every layer above checks before calling through the table. **Verified here**: `vkGetInstanceProcAddr` answers NULL for instance-level commands with `VK_NULL_HANDLE` in hand (`vkDestroyInstance` → NULL) and answers non-NULL for the same name through a live instance — which is why `xud_instance.c` resolves `vkDestroyInstance` at destroy time, not at loader time.

**Verified instance bring-up (2026-10-16)** — the shape `src/xud/xud_instance.c` uses:
- `vkEnumerateInstanceExtensionProperties` has **no count-only form**: `pCount` is input/output (capacity in, written count out). Verified here: a zero-capacity call writes 0 and returns `VK_INCOMPLETE`, so the count cannot be read without an array — hence the doubling-capacity query (16 → 512 max) in `xud_instance_query_extensions`. `VK_INCOMPLETE` is **not** an error in Vulkan but means "the list was cut", so `xud_result_from_vk` maps it to `XUD_VULKAN_ERROR`, never `XUD_OK`.
- lavvk advertises **26 instance-level extensions**, and all three xudlibc wants are among them: `VK_KHR_get_physical_device_properties2`, `VK_KHR_surface`, `VK_KHR_portability_enumeration`. `vkCreateInstance` at `apiVersion = VK_API_VERSION_1_0` with those three enabled returns `VK_SUCCESS` here. `VK_INSTANCE_CREATE_FLAG_PORTABILITY_BIT` is deliberately **not** set — it tells a driver it *may hide* portability-gated extensions, which is the opposite of what this library wants; requesting the enumeration extension is the honest request.
- Validation layers are requested **only through the diagnostics struct** (`xud_instance_create(&instance, loader, diagnostics)`): with it, `enabledLayerCount` is what the loader advertised (3 here) and the diagnostics layer's wanted extensions are appended after this layer's own three; with `diagnostics == NULL` (a release build, `XUD_VALIDATE == 0`) the count is 0 and nothing is requested. `xud_instance.c` enables no name it has not seen advertised, in either case. **Verified here**: `vkCreateInstance` with the diagnostics struct returns `VK_SUCCESS` with 4 extensions enabled (`VK_KHR_get_physical_device_properties2`, `VK_KHR_surface`, `VK_KHR_portability_enumeration`, `VK_EXT_debug_utils`) — `VK_EXT_validation_features` is **not** advertised here, so it stays out and the `pNext` chain stays empty (a `VkValidationFeaturesEXT` may only ride the chain of an extension that is enabled).

**Verified diagnostics bring-up (2026-10-16)** — the shape `src/xud/xud_diagnostics.c` uses:
- `vkEnumerateInstanceLayerProperties` is **global-level** (no handle in hand) and has the same input/output `pCount` shape as the instance-extension query — no count-only form, so the layer query doubles capacity 16 → 512 exactly like `xud_instance_query_extensions`. **3 layers advertised** here: `VK_LAYER_MESA_device_select`, `VK_LAYER_MESA_anti_lag`, `VK_LAYER_KHRONOS_validation`; all three are enabled, in advertised order, never by name.
- The two messenger entry points are **resolved on demand through a live `VkInstance`** (`xud_loader_resolve`), never cached in a loader slot: `vkGetInstanceProcAddr` answers NULL for `vkCreateDebugUtilsMessengerEXT`/`vkDestroyDebugUtilsMessengerEXT` unless `VK_EXT_debug_utils` is enabled on the instance in hand, and a resolve cached across an instance recreation would be a stale pointer into a layer that is gone. `VK_EXT_debug_utils` **is** among the 26 instance-level extensions lavvk advertises, so `xud_diagnostics_create_messenger` returns a live messenger here.
- `vkDestroyDebugUtilsMessengerEXT` takes **three** arguments in these headers — `(VkInstance, VkDebugUtilsMessengerEXT, const VkAllocationCallbacks *)` — and the owning `VkInstance` must still be alive at the call, which is why `xud_close_context()` destroys the messenger inside `xud_instance_destroy()` *before* the instance goes away, and `xud_diagnostics_close()` afterwards only frees the struct.
- The log callback counts into the struct it was handed as `pUserData` and **counts nothing when `pUserData` is NULL** (there is no struct to count into — the counters cannot travel), and it must survive a NULL `pMessage`/`callbackData`. `VK_FALSE` is returned always: a message never aborts a driver.
- **What is deliberately not done**: `VkDebugUtilsMessengerCreateInfoEXT` on the `VkInstanceCreateInfo` `pNext` chain. This loader segfaults inside `vkCreateInstance` with a messenger on that chain (probed twice, 2026-10-16); the standalone `vkCreateDebugUtilsMessengerEXT`/`vkDestroyDebugUtilsMessengerEXT` pair above works cleanly, and `VkValidationFeaturesEXT` on that chain **is** accepted (probed: `VK_SUCCESS`).

**Verified physical-layer header facts (2026-10-18)** — grepped in `/usr/include/vulkan/vulkan_core.h` (Vulkan-Headers 1.4.357) and measured by compiling a `sizeof` TU; **no driver call involved**, which is why `src/xud/xud_physical.h` may state them and its `.c` must re-probe anything it wants about a driver:
- The `*2` property structs are all present: `VkPhysicalDeviceProperties2` (line 5809), `VkPhysicalDeviceFeatures2` (5803), `VkPhysicalDeviceMemoryProperties2` (5843) — each is `{ VkStructureType sType; void *pNext; <core struct> core; }`, so the fields read back out of them are the **core** names (`caps.properties.apiVersion`, `caps.features.features.sampler2D`, …). `VkPhysicalDeviceValidationFeatures` / `VK_VALIDATE_*` remain absent (see the header gap below) — the `*2` trio here is unrelated to that gap.
- **Naming trap**: the queue-family wrapper is `VkQueueFamilyProperties2` (5837) — **no `PhysicalDevice` prefix**, unlike the other three — and its core struct is `VkQueueFamilyProperties` (5827). `VkPhysicalDeviceQueueFamilyProperties` is a *different* thing: a `VkPhysicalDeviceProperties2` `pNext` chain entry (5821). The three names sit within 20 lines of each other; `xud_physical.h`'s file-top comment records this so nobody reaches for the wrong one.
- Device-level PFN typedefs present: `PFN_vkEnumerateDeviceExtensionProperties` (4544, takes `pLayerName` — NULL means the driver's own list) and `PFN_vkGetPhysicalDeviceQueueFamilyProperties2` (6182). Both have an input/output `pCount` and **no count-only form**, so both queries in `xud_physical.c` follow the doubling-capacity shape `xud_instance_query_extensions` and `xud_diagnostics_query_layers` already use (16 → 512 max here).
- **No `VK_QUEUE_PRESENT_BIT` anywhere**: grepped `VK_QUEUE_PRESENT` across the whole `/usr/include/vulkan/` tree — **zero hits**. The queue flags are `VK_QUEUE_GRAPHICS_BIT` 0x1, `COMPUTE` 0x2, `TRANSFER` 0x4, `SPARSE_BINDING` 0x8, `PROTECTED` 0x10, plus vendor bits (`VIDEO_DECODE`/`VIDEO_ENCODE_KHR` 0x20/0x40, `OPTICAL_FLOW_NV` 0x100, `DATA_GRAPH_ARM` 0x400). Presentation is **not** a queue flag: it is a per-window-system query (`vkGetPhysicalDeviceWaylandPresentationSupportKHR`, `Xcb`, `Xlib`, `Win32`, `DirectFB`, `UBM`, `Screen` — system-specific, no core form), i.e. GLFW's `glfwGetPhysicalDevicePresentationSupport` at S7. So the one-family rule is enforced by a **graphics** check here plus GLFW's presentation check there, never by a bit that does not exist.
- Sizes as compiled here: `VkPhysicalDeviceProperties2` 840 B, `VkPhysicalDeviceFeatures2` 240 B, `VkPhysicalDeviceMemoryProperties2` 536 B, `VkQueueFamilyProperties2` 40 B, `VkExtensionProperties` 260 B (= `VK_MAX_EXTENSION_NAME_SIZE` 256 + one `uint32_t`, no padding — which is what the caps' heap extension array assumes, and `static_assert`s). `VkPhysicalDevice` is 8 B (a pointer, `VK_USE_64_BIT_PTR_DEFINES == 1`, already asserted in `xud_loader.c`). `XudDeviceCaps` embeds the three `*2` structs + the chosen queue row ⇒ ~1.7 KB, so `XudPhysicalDevice` is heap-allocated, never a stack temporary.
- Constants the caps storage keys on: `VK_MAX_PHYSICAL_DEVICE_NAME_SIZE` 256, `VK_UUID_SIZE` 16, `VK_MAX_MEMORY_TYPES` 32, `VK_MAX_MEMORY_HEAPS` 16; `VK_API_VERSION_1_0` 4194304, `VK_API_VERSION_1_1` 4198400, `VK_API_VERSION_1_4` 4210688; `VK_PHYSICAL_DEVICE_TYPE_OTHER` 0 … `INTEGRATED_GPU` 1, `DISCRETE_GPU` 2, `VIRTUAL_GPU` 3, `CPU` 4.
- **No new loader slot is needed by this header** — `XUD_LOADER_SLOT_COUNT` stays 19. The six physical-layer names were already slotted with S5.1's `PFN_vkGetPhysicalDeviceProperties2` bump; `xud_physical.h` declares no PFN and binds nothing.

**Verified physical-layer pure facts (2026-10-18)** — what `src/xud/xud_physical.c` (S5.3, the pure half only) states with **no driver call**, so the headless run covers it completely:
- **Member-path trap, the one thing that broke a build here**: from the caps the apiVersion is **two** hops, `caps->properties.properties.apiVersion` (the caps field is itself named `properties`, its type is the `VkPhysicalDeviceProperties2` wrapper), from a bare `VkPhysicalDeviceProperties2` variable it is **one**, `props[i].properties.deviceType`. Writing the two-hop form against a one-hop variable is the gcc error `'VkPhysicalDeviceProperties' has no member named 'properties'; did you mean 'sparseProperties'?`. Same for features: `caps->features.features.robustBufferAccess`.
- **The wanted feature list is three names**, each wanted for a stated reason and never forced on: `robustBufferAccess` (absence is a correctness hazard — out-of-range index reads are undefined, not clamped, and the batch layer will not bounds-check every draw), `samplerAnisotropy` (ground truth for the "check before you use" rule — widely absent, so the sampler variant gates on the caps), `wideLines` (raylib's `DrawLine3D` width, absent on most drivers, so lines fall back to width 1 rather than failing). The intersection writes every `VkPhysicalDeviceFeatures` field in struct order and **normalises the output to VK_TRUE/VK_FALSE** (a driver answering 42 for a feature must not have 42 handed to `VkDeviceCreateInfo`); the driver's verbatim answer stays in `caps->features`.
- **The queue-family rule is the graphics bit only** (there is no present bit — see the S5.2 grep): first family in driver order whose `queueFlags` carry `VK_QUEUE_GRAPHICS_BIT` wins, its whole `VkQueueFamilyProperties2` row is copied once into the caps, and refusal writes `XUD_PHYSICAL_NO_QUEUE_FAMILY` **plus a zeroed row** — a row still carrying the rejected family's flags is what S6 would read as a queue to create. Transfer/compute/granularity are recorded, never required.
- **`apiVersion` is read back verbatim even when the floor check fails** (`caps->apiVersion = caps->properties.properties.apiVersion` before the compare with `XUD_INSTANCE_API_FLOOR`), so a rejection can be diagnosed; a caps struct still at its zero state answers 0, which fails the floor — that is the "the struct still says nothing" signal the four void `*2` queries give (see the header-facts block).
- **`xud_physical_close` lands with the pure half**, not with the driver half, because the point of landing it is the sanitizer run of a heap caps struct + its extension array with no driver involved: it frees the caps' one heap member, clears the pointer and both counts, frees the struct, answers `XUD_ALREADY` on a NULL-in-handling pointer, and calls no driver function (there is no `vkDestroyPhysicalDevice` — see the header-facts block).
- **One assert removed on the way**: `sizeof(XUD_PHYSICAL_WANTED_FEATURES) == sizeof(VkPhysicalDeviceFeatures)` looked like the item's "wanted ∩ driver" guarantee but both spellings are the **same typedef** (`VkPhysicalDeviceFeatures2` is the wrapper, its `.features` member is the core struct), so it was a tautology; the real compile-time check is the designated initializers, which fail if the wanted struct is ever spelled as the `...2` wrapper.
- **`xud_physical.c` is compiled into the `xud_test` executable, not the archive** (S5.6 adds the archive line when the contract is complete): the pure half exists, the driver half does not, so the only way to test these functions at all is to build the TU into the test binary. `ar -t build/libxud.a` still lists **5** objects and does not list `xud_physical.c.o`; the link is still Vulkan-free.
- Test counts after this step: headless **339 checks / 0 failures** (was 281), `XUD_GPU_TESTS=1` **401 / 0** (was 343), `-DXUD_SANITIZERS=ON` green both ways.

**Verified physical-layer driver facts (2026-10-18)** — what `src/xud/xud_physical.c` (S5.4, the driver half) states about **this** driver, probed by running it through the validation layer:
- **The machine answers one physical device**: `vkEnumeratePhysicalDevices` returns 1 candidate, `apiVersion = 4211042` (= `VK_MAKE_API_VERSION(0,1,4,363)`), `deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU`, vendor 0x8086 / device 0x400, name "Intel Iris Graphics … (TigerLake-LP GT2 …)". Limits read into the caps here: `maxUniformBufferPerStageSize` 65536, `maxStorageBufferRange` 4294967295, **`maxPushConstantsSize` 128** (the guaranteed floor — the push-constant struct of the design sketch must fit it, and this driver gives no headroom), `maxBufferSize` 4294934527, `maxVertexInputAttributes` 32, `maxDescriptorSetUniformBuffers` 65536, **`maxCombinedImageSamplerDescriptorCount` 1** (so a set-1 sampler array of more than one is gated on caps, not assumed), `maxComputeWorkgroupSize` 1024, `maxImageDimension2D` 16384. Memory: 3 heap types / 3 heaps, smallest `UNIFORM_BUFFER` 16384 / largest 4294967295, `VK_MEMORY_HEAP_DEVICE_LOCAL` 4294967295 B, `VK_MEMORY_TYPE_UNIFORM_BUFFER` size 16384 align 256.
- **One queue family**, `queueFlags = 0x1f` (GRAPHICS|COMPUTE|TRANSFER|SPARSE_BINDING|PROTECTED), `queueCount = 1`, `maxImageSampleCount = 1`, `timestampValidBits = 36`. The graphics rule picks family 0; there is no second family to prefer, so the "exactly one family" rule is not exercised by a choice here — it is exercised by the synthetic two-family row in the headless pure test.
- **196 device-level extensions** through a NULL `pLayerName` (the driver's own list), `VK_KHR_swapchain` among them — the caps' heap array is sized to what the query wrote, 196 × 260 B here.
- **The validation layer is a hard signal, not a suggestion**: an unstamped `VkQueueFamilyProperties2` row handed to `vkGetPhysicalDeviceQueueFamilyProperties2` answers `vkGetPhysicalDeviceQueueFamilyProperties2KHR(): pQueueFamilyProperties[i].sType must be VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2` — for **every row of the capacity passed in, not only the rows the driver writes** (the message stream stops at 10 duplicates). So the queue-family query stamps the whole array before the call, and the three single-row `*2` getters stamp their one row. A `calloc`-ed row is not a substitute for stamping: the wrapper's `sType` is the only thing the driver reads to know what it is being handed.
- **The `*2` wrappers must be stamped and their `pNext` cleared before the getter, and the getter's `pNext` write-back must be checked after it**: lavvk answers `VK_SUCCESS` with `pNext` still NULL for all three (`properties`, `features`, `memoryProperties`) — no `pNext` struct is chained into the caps here today, so S6's `VkDeviceCreateInfo` chain stays empty. A driver that chains something xudlibc does not model is refused at the caps rather than silently dropped — that check is what keeps the "recorded caps are complete" promise honest, and it is why the caps' three `pNext` fields are public.
- **`xud_loader_bind()` cannot take a `const XudLoader *`**: it writes slots, and both compilers reject passing `const XudLoader *` to `XudLoader *` (`discards const qualifier`), as they reject `&loader->slot` (a `const PFN_vkX *`) into the `void *` out-param of `xud_loader_resolve`. So `xud_physical_open()` takes `XudLoader *loader` **non-const** (the loader is the context's one mutable table), while `xud_physical_query_devices()` — which binds nothing — keeps `const XudLoader *`.
- **`xud_test_gpu_physical()` runs inside `xud_test_gpu_instance()` before the instance is destroyed**: the physical device is owned by the instance, so its teardown must happen while the owner is alive, exactly like the diagnostics messenger's teardown inside `xud_instance_destroy()`.
- Test counts after this step: headless **361 checks / 0 failures** (was 339), `XUD_GPU_TESTS=1` **463 / 0** (was 401), `-DXUD_SANITIZERS=ON` green both ways (361 / 463, no sanitizer report), and `ar -t build/libxud.a` lists **6** objects with `xud_physical.c.o` among them while `nm -u build/xud_test` still lists no `vk*` symbol and `ldd` still lists no `libvulkan`.
- **Vulkan-Headers moved 1.4.357 → 1.4.363 on this machine during the physical step** (`VK_HEADER_VERSION 363`, `libvulkan.so.1 → libvulkan.so.1.4.363`, `vulkan-validation-layers 1.4.363.0-1`); every line number grepped in `vulkan_core.h` for the physical layer (5809/5803/5843/5837/5827/5821/4544/6182) is identical in both versions, so the header-facts block above stays true.

**Verified device-layer facts (2026-10-18)** — what `src/xud/xud_device.h`/`src/xud/xud_device.c` (S6) state. The header facts were grepped in `/usr/include/vulkan/vulkan_core.h` (1.4.363) with **no driver call**; the rest was probed by running the layer through the loader, with and without `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`:
- **The layer is named `xud_device`, not `xud_device_queue`**: a queue cannot exist without the device that owns it — `PFN_vkGetDeviceQueue` (4547) takes a `VkDevice`, and grepping `vkDestroyQueue` across `/usr/include/vulkan/` gives **zero hits**, so there is no queue teardown rung to own. `XudDevice` is the owner (handle + the create-info the driver was handed + the queues it resolved); `XudQueue` is a **record** of a queue (`VkQueue` + the family/index pair it came from), not an owning type — which is why `xud_device_queue_get()` is pure and `xud_device_close()` frees no queue.
- **Queue roles are a library decision, not Vulkan state**: `XUD_QUEUE_ROLE_GRAPHICS` / `XUD_QUEUE_ROLE_PRESENT` map two roles onto queues of the **one** family the physical layer chose. There is no `VK_QUEUE_PRESENT_BIT` (see the S5.2 grep) and no second family here to map a present role onto, so the roles are how the layer above asks for a queue, and on this machine both resolve to the same `VkQueue` handle.
- **Header facts**: `VkDeviceQueueCreateInfo` is at **3727** — the plan's `VkQueueCreateInfo` does not exist (grep that name: zero hits) — fields `sType`/`pNext`/`flags`/`queueFamilyIndex`/`queueCount`/`pQueuePriorities`; `VkDeviceCreateInfo` at **3736**, with `queueCreateInfoCount`/`pQueueCreateInfos` and the two **legacy** members the headers themselves comment as "not used" (`enabledLayerCount`, `ppEnabledLayerNames` — instance-level layers belong to `VkInstanceCreateInfo`, so this layer writes `0`/`NULL`). Stamps `VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO = 2` (209) and `VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO = 3` (210) — distinct, asserted. `VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT = 0x00000001` (2915) is the only queue-flag bit here and stays **off**. PFN lines: `vkGetDeviceProcAddr` 4540, `vkCreateDevice` 4541, `vkDestroyDevice` 4542, `vkGetDeviceQueue` 4547, `vkDeviceWaitIdle` 4550.
- **`PFN_vkCreateDevice` takes four arguments** — `(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*)` — the physical handle is the first, so the device layer's whole input is the physical layer's recorded caps. **`PFN_vkDestroyDevice` and `PFN_vkGetDeviceQueue` return `void`**: no `VkResult` for `xud_result_from_vk` to map, so the queue lookup uses zero-state-before-the-call (`*pQueue = VK_NULL_HANDLE` first) and checks after it — a `VK_NULL_HANDLE` out of a live call is `XUD_VULKAN_ERROR`, not success. `PFN_vkDeviceWaitIdle` **does** return `VkResult`.
- **Device-level names are instance-level for resolution, and cached**: probed through `xud_loader_resolve` — `vkCreateDevice`, `vkDestroyDevice`, `vkGetDeviceQueue`, `vkDeviceWaitIdle` all answer `XUD_OK` through a live `VkInstance` and `XUD_UNSUPPORTED` with `VK_NULL_HANDLE` in hand, so `xud_device_open()` binds all four before its first call and teardown calls through the cached slot rather than resolving again. **No new loader slot was needed** — all four were already in the table since S2, `XUD_LOADER_SLOT_COUNT` stays **20**. `vkGetDeviceProcAddr` is the one device-level name **not** slotted and not called here: it needs the `VkDevice` itself, not the instance, and nothing in this tree resolves a device-level entry point through a device yet — that is the renderer layer's job.
- **The validation layer is what catches the shapes this layer must not write** (the driver accepts almost all of them silently — probed: `queueCount+1`, duplicate family rows, unstamped create-infos, `queueCount = 0`, no queue rows at all, priorities `0.0`/`2.0`, the protected flag all answer `VK_SUCCESS` with a live device without the layer). With the layer on, each is a hard message: `pQueuePriorities is NULL` (`VUID-VkDeviceQueueCreateInfo-pQueuePriorities-parameter`) for any `queueCount > 0`, so the row's `pQueuePriorities` must point at storage that outlives the call — `XudDevice` keeps its own `queuePriorities[]` beside the row for exactly that; `queueFamilyIndex (99) is not one of the queue families given via VkDeviceQueueCreateInfo` (`VUID-vkGetDeviceQueue-queueFamilyIndex-00384`); `queueIndex (1) is not less than the 1 queues requested` (`VUID-vkGetDeviceQueue-queueIndex-00385`); `Total queue count requested from queue family index 0 is 2, which is greater than queue count available (1)`; `pQueueCreateInfos[1] uses the same queueFamilyIndex (0) and flags as [0]`; `pCreateInfo->sType must be VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO`; `pQueueCreateInfos[0].sType must be VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO`; `queueCount must be greater than 0`. The row is therefore built from `caps.chosenQueueFamilyProperties` with `queueCount = min(family.queueCount, XUD_DEVICE_QUEUE_MAX_COUNT)` and never more than the family rowed.
- **Check-before-you-use, observed at the driver**: an unadvertised device extension answers `VK_ERROR_EXTENSION_NOT_PRESENT` (`-7`) with no device, and an absent feature asked for by name answers `VK_ERROR_FEATURE_NOT_PRESENT` (`-8`) — both map to `XUD_UNSUPPORTED` through `xud_result_from_vk`. The enabled-feature struct handed to `VkDeviceCreateInfo` is `xud_physical_record_features()`'s **intersection**, never `caps.features.features` verbatim: on this driver `robustBufferAccess`/`samplerAnisotropy` come out VK_TRUE and `wideLines` VK_FALSE, and `geometryShader` — which lavvk has but the library does not want — stays VK_FALSE in the create info. The enabled extension list is built only from `caps.deviceExtensionProperties` (196 advertised here, `VK_KHR_SWAPCHAIN` at row 84), so exactly **one** name is enabled here and `VkDeviceCreateInfo.pNext` stays NULL because the caps' three `*2` chains came back empty (see the S5.4 bullet).
- **Teardown is guard → destroy → free**: `xud_device_close()` calls `xud_device_wait_idle()` (the plan's `xud_wait_idle`, named after the layer per the repo rule) before `vkDestroyDevice` through the cached slot, then frees the struct; a lost device is **still destroyed** — skipping `vkDestroyDevice` would leak an object the driver still holds — and `XUD_LOST` from the guard is still propagated. `PFN_vkDeviceWaitIdle` on a device with nothing in flight answers `VK_SUCCESS` here, so the guard is not a no-op we can skip.
- **`xud_test_device_gpu()` runs inside `xud_test_gpu_physical()`, before the physical device is closed and inside `xud_test_gpu_instance()`**: a `VkDevice` is created from a `VkPhysicalDevice` handle, so its teardown must happen while its owner is alive — the same rule the physical and diagnostics tests follow, and the order `xud_close_context()` uses. It opens a device, reads the row/features/extensions back, resolves both role queues, waits idle, closes, and re-opens through the same cached slots (the slots are cached, so a recreation path works).
- Test counts after this step: headless **465 checks / 0 failures** (was 361), `XUD_GPU_TESTS=1` **615 / 0** (was 463) and validation-clean with `VK_INSTANCE_LAYERS` unset **and** forced to `VK_LAYER_KHRONOS_validation` (the only `ERROR/validation` lines in either run are the diagnostics layer's own synthetic headless probe), `-DXUD_SANITIZERS=ON` green both ways (465 / 615, no report), `ar -t build/libxud.a` lists **7** objects with `xud_device.c.o` among them, `nm -u build/xud_test` still lists no `vk*` symbol and `ldd` still lists no `libvulkan`.
- **Honest boundary after this step**: the device layer is implemented, in the archive, and tested against the driver — but **nothing in the context calls it yet** (`xud_core.h`'s `device` field is still a `void *` sentinel, `xud_init_context` still answers `XUD_UNSUPPORTED` for valid args). Wiring the context to it, and the surface/swapchain below it, is S8's ownership step.

**Verified render-pass decision facts (2026-10-22)** — probed today by two scratch programs linked against `build/libxud.a` (loader → instance → physical, then deleted; **no library file changed and no render pass exists in the tree yet** — **superseded 2026-10-25, S11**: the explicit path this block settles is in the tree now, see *Verified first-triangle facts*). These are the ground the swapchain and renderer steps are written against, and the reason the render-pass decision below is settled rather than left open:
- **Both paths are reachable on this driver, so a caps gate between them is untestable here.** `apiVersion = 4211042` (core 1.4) and among the **196** advertised device-level names sit `VK_KHR_dynamic_rendering`, `VK_KHR_dynamic_rendering_local_read`, `VK_KHR_create_renderpass2`, `VK_EXT_dynamic_rendering_unused_attachments`, `VK_EXT_conditional_rendering`, `VK_EXT_extended_dynamic_state` 1/2/3, `VK_EXT_vertex_input_dynamic_state`, `VK_KHR_swapchain` (+`maintenance1`, `mutable_format`). Chaining the **core** `VkPhysicalDeviceDynamicRenderingFeatures` into a fresh `vkGetPhysicalDeviceFeatures2` answers `dynamicRendering = VK_TRUE`. "Gate both paths on caps" would therefore ship one half that has never met a driver — against the testing rules — so the tree picks one path and keeps the other as a written-down future gate.
- **The chained-struct trap, and why the explicit path is the cheap one**: that same probe leaves `feats.pNext` **non-NULL after** the call when a struct was chained in, while the three unchained `*2` getters leave `pNext` NULL — which is exactly what `xud_physical.c`'s "the driver chained something we did not ask for" check relies on today. A dynamic-rendering path must restate that check as "only the chain we provided", record a 4th chained struct in `XudDeviceCaps`, and grow `VkDeviceCreateInfo.pNext` from NULL in `xud_device.c`: it touches **two landed contracts**, not just the renderer. The explicit-pass path touches neither.
- **MSAA is not absent here**: `limits.framebufferColorSampleCounts` / `framebufferDepthSampleCounts` / `framebufferNoAttachmentsSampleCounts` all answer **0x1f** (1, 2, 4, 8, 16 samples). Gate MSAA on those names, **never** on the queue family's `maxImageSampleCount` (the S5.4 bullet records 1 for the chosen family): that number comes from `VkPhysicalDeviceQueueFamilyProperties2`, a `pNext` chain entry the physical layer does **not** record today, and it is a per-family image-creation limit, not a framebuffer sample limit. An in-pass resolve attachment — which only the explicit path has — is a real option on this driver.
- **Header facts** (`vulkan_core.h`, 1.4.363): every name either path needs has both a `VKAPI_ATTR` declaration and a `PFN_` typedef — checked for 40 names, all present. Stamps `VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO = 37` (244) and `VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO = 38` (245), distinct; create-infos at `VkFramebufferCreateInfo` 4436, `VkSubpassDependency` 4448, `VkSubpassDescription` 4458, `VkRenderPassCreateInfo` 4471, `VkRenderPassBeginInfo` 4528 (the `vkCmdBeginRenderPass` argument, core 1.0), `VkRenderPassCreateInfo2` 6949 (the renderpass2 upgrade, extension-gated). **Naming trap**: the recording pair is `vkBeginCommandBuffer` (PFN 4587) / `vkEndCommandBuffer` (4588) — **no `Cmd` prefix**, unlike every other command-buffer-level name; `vkCmdBeginCommandBuffer` does not exist (grepped: zero hits). The dynamic trio sits at `VkRenderingAttachmentInfo` 7915, `VkRenderingInfo` 7928, `VkPhysicalDeviceDynamicRenderingFeatures` 7951, PFN pair `vkCmdBeginRendering`/`vkCmdEndRendering` 7989/7990, `VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME` 10294.
- **Loader-slot cost of the choice**: the explicit path needs **6** new device-level slots (`vkCreateRenderPass`, `vkDestroyRenderPass`, `vkCreateFramebuffer`, `vkDestroyFramebuffer`, `vkCmdBeginRenderPass`, `vkCmdEndRenderPass`, plus `vkCmdNextSubpass` if multi-subpass is ever used); the dynamic path needs **2** (`vkCmdBeginRendering`, `vkCmdEndRendering`). `XUD_LOADER_SLOT_COUNT` stays **20** today — none of them is added yet. `vkGetDeviceProcAddr` (PFN 4540) is still **not** slotted and is the first slot the renderer step must add: everything below the device resolves through it, not through the instance. **(Superseded 2026-10-23, see *Verified Volk bring-up*: there is no slot to add — those names are rows of Volk's `VolkDeviceTable`, filled in bulk by `xud_loader_bind_device()`, and `xud_loader_bind_device()` already requires the `instanceTable.vkGetDeviceProcAddr` row it resolves them through. What the renderer step must still do is check each row before calling it.)**

**Verified header gap:** `VkPhysicalDeviceValidationFeatures` / `VK_VALIDATE_*` are **not in Vulkan-Headers at all** (SDK-only). The only validation-feature struct available here is `VkValidationFeaturesEXT` (a `pNext` chain on `VkInstanceCreateInfo`), plus `VK_VALIDATION_FEATURE_ENABLE_*` / `VK_VALIDATION_FEATURE_DISABLE_*` enums from `vulkan_ext` headers — `xud_diagnostics.c` asks for `VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT` and `VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`. Confirm with a grep before using any of these names.

## Commands

| Task | Command |
| --- | --- |
| Configure | `cmake -B build -G Ninja` (capital **N** — cmake 4.4.3 rejects `-G ninja`) |
| Build everything | `ninja -C build` |
| Build one example | `ninja -C build <target>` |
| Syntax/lint check (fast, no codegen) | `gcc -fsyntax-only -std=c11 -Wall -Wextra -Wpedantic -Werror -I src/xud -I src/external src/xud/*.c` (**`-I src/external` is required** — every Vulkan-bearing xud header includes `"volk.h"` before `<vulkan/vulkan.h>`, and `volk.h` lives in `src/external/`) |
| Format sources | `./scripts/format.sh` (clang-format over `src/xud tests examples` only — never `src/external/`, never a bare `src`); `./scripts/format.sh --check` is the form that can gate a commit |
| Unit tests (headless) | `./build/xud_test` |
| GPU tests | `XUD_GPU_TESTS=1 ./build/xud_test` |
| Sanitizer build | `cmake -B build-san -G Ninja -DXUD_SANITIZERS=ON` then `./build-san/xud_test` (adds `-fsanitize=address,undefined`) |
| Second opinion (clang 22) | `cmake -B build-clang -G Ninja -DCMAKE_C_COMPILER=clang` then `ninja -C build-clang` (the vendored GLFW/Volk set must be warning-free here too — verified 2026-10-23) |
| Build the shaders | automatic: `ninja -C build` runs the `xud_generated_headers` step (`glslangValidator -V` → `build/gen/*.spv` → `spirv-val` → `scripts/spv_to_header.cmake` → `build/gen/xud_solid_spv.h`) |
| Compile one shader by hand | `glslangValidator -V -o build/gen/solid_vert.spv shaders/solid.vert` (and `-G` for geometry/comp) |
| Validate SPIR-V | `spirv-val build/gen/solid_vert.spv` (the same rule the build already runs) |

Keep `build/`, `bin/` and `reference/` out of git (`.gitignore` already covers all three). Never commit generated `.spv` blobs unless a test needs a checked-in SPIR-V fixture — say why.

## Reference material (what is actually here)

`reference/raylib/` — **raylib 6.2 headers**, copied 2026-10-16 from `~/repos/xudlib/reference/raylib/src/` (git `6.2-5-gc757c743`, i.e. 6.2 + 5 commits past the tag). Read-only, gitignored, 560 KB total. `reference/raylib/PROVENANCE.md` records the source and per-file contents.

| File | Use it for |
| --- | --- |
| `reference/raylib/raylib.h` | The whole public API surface — every function, type, enum, struct. This is the shape `xud.h` mirrors. |
| `reference/raylib/raymath.h` | Matrix/vector math (header-only, column-major `Matrix` = 16 floats) — model of `xudmath.h` |
| `reference/raylib/rcamera.h` | Camera 3D/2D + camera modes / freecam movement |
| `reference/raylib/rgestures.h` | Touch gesture types |
| `reference/raylib/rlgl.h` | raylib's GL abstraction layer — the immediate-mode→draw-batch structure that our Vulkan `xudrl` layer replaces |
| `reference/raylib/config.h` | raylib's build/config switches |
| `reference/raylib/LICENSE` | raylib license text, must stay with the material |

**Not copied**: the `.c` module sources (`rcore.c`, `rshapes.c`, `rtext.c`, `rtextures.c`, `rmodels.c`, `raudio.c`, `rglfw.c`) and `src/external/` (cgltf, miniaudio, stb, glfw, …). Ask before pulling either in — `src/external/` in particular is a vendoring decision, not a reference read.

## C house style

- **C11**, one public header (`xud.h`) plus `xud*.h`/`xud*.c` module headers/sources. No C++ anywhere.
- Names: `xudPascalCaseOrNot` — concretely: **functions `xudSnakeCase`** (`xud_init_window`, `xud_draw_cube`), **types `PascalCase`** (`XudWindow`, `XudCamera3D`), **macros/constants `XUD_SNAKE_UPPER`** (`XUD_MAX_LIGHTS`). No Hungarian prefixes.
- Every public function returns an error code (`XudResult`) or fills an out-param; **nothing is silently ignored**.
- One `XudContext` owns instance/device/swapchain/allocator. Callers never hold a `VkDevice`, `VkSwapchainKHR`, or raw `VkBuffer`.
- Opaque handles = `{ uint32_t index; uint32_t generation; uint32_t kind; }` validated against a slot array. Stale handles must be caught, not UB. **Decided 2026-10-23**: this is the currency the **public** API hands out — raylib's pointer/GL-id-in-a-struct form is not adopted, and the `kind` tag is what makes cross-kind misuse a caught runtime error rather than a layout coincidence (see *Settled decisions*).
- Cleanup idiom: `goto fail;` ladder in init paths, one `xud_close_*()` teardown; every partial-failure path must leak nothing.
- No global mutable state except the single library context pointer; multiple windows must work.
- `#include` guard `XUD_<FILE>_H_`; file-top comment stating what the file owns and which layer it belongs to.
- All blocks must have curly braces. Even one line if statements.
- Use clang-format before commit.
- Doc comments: `///` Doxygen-style above public declarations only (no Doxygen build required yet).
- Keep comments short. One line is enough. Only put huge block comments on public API. Keep comment lines 80 char long. If you have lines over 100 lines, fix them yourself. Don't try to make a python script. You are best at NLP tasks.
- Compile flags in dev: `-std=c11 -Wall -Wextra -Wpedantic -Werror` plus `-fsanitize=address,undefined` for tests. Release: `-O2`. The CMakeLists adds these itself (`-Wall -Wextra -Wpedantic -Werror` always, `-g -DXUD_DEBUG=1` by default, `-O2` under `-DXUD_DEBUG=OFF`, sanitizers under `-DXUD_SANITIZERS=ON`).
- **Never add `-Ddefault=lib`** to a compile line here: with gcc 16.2.1 and clang 22.1.8 it is a preprocessor define of a macro named `default`, which rewrites every `default:` label in every `switch` and breaks the build (verified: both compilers fail a two-case switch under it).
- Under these flags, `dlsym` → function-pointer casts and message-less `static_assert` are **errors, not warnings** — the cast rule still applies if anything ever resolves a name by hand (see *Verified loader bring-up* for the union+`memcpy` idiom that the replaced loader used); nothing in the tree does one now, Volk's tables hand back typed PFN slots. The `static_assert(cond, "why")` form is what every header here uses.

## Vulkan rules (best practices — these are the point of the repo)

- **No Vulkan symbols linked at build time.** Load `libvulkan.so` with `dlopen` + `vkGetInstanceProcAddr` (Volk-style), resolve entry points into a small internal function table. Rationale: the library must build and run on a machine with no driver, and `xud_vulkan_supported()` must be a real runtime query. Do **not** add `-lvulkan` to the link line. **Vendored Volk (`src/external/volk.h`/`volk.c`) is what does this today**: `src/external/volk.c` is compiled into the `xud` archive, `xud_loader.c` wraps Volk's instance/device tables in the context's one `XudLoader`, and under `VK_NO_PROTOTYPES` every entry point is a **data pointer**, so the rule is a link-time fact rather than a convention — `nm -u build/xud_test` lists no `vk*` symbol and `ldd` lists no `libvulkan`. The hand-rolled 20-slot table it replaced is gone (see *Verified Volk bring-up* and *Loader rework* below).
- **Never hardcode a layer name.** Enumerate with `vkEnumerateInstanceLayerProperties`, request everything advertised, and re-create the instance with `ppEnabledLayerNames`. Validation is gated: `XUD_VALIDATE` on in debug builds, off in release, always overridable by env (`VK_INSTANCE_LAYERS`) or config.
- **Check before you use.** Every extension and feature is queried (`vkEnumerateDeviceExtensionProperties`, `vkGetPhysicalDeviceFeatures2`) and recorded in a `XudDeviceCaps` struct; `VkDeviceCreateInfo` carries **only** what was verified. Anything unverified stays off and the feature that needed it returns `XUD_UNSUPPORTED`.
- **Explicit pipelines, created once at init.** A small fixed set of variants (see Design sketch), never created per draw, never rebuilt per frame.
- **Allocate once, suballocate always.** One GPU-memory allocator owns all buffers/images; static geometry is a one-time allocation, per-frame data comes from a ring/transient allocation. **No `vkCreateBuffer` inside a frame.** The allocator is **ours** — no VMA, settled 2026-10-23, see *Settled decisions*.
- **Staging via transfer ops only**: `vkCmdCopyBuffer` / `vkCmdCopyImage` / `vkCmdCopyBufferToImage` from a staging ring. No `vkMapMemory` per frame without a matching `vkUnmapMemory` + `VkBufferInvalidateRangeInfo` (or a `vkCmdPipelineBarrier` with the right scope), and prefer the copy path over mapped writes for anything transient.
- **Synchronization is classic**: acquire/present semaphores for frame boundaries, one `VkFence` per swapchain image, `vkCmdPipelineBarrier` and explicit subpass dependencies inside a command buffer. **No `VK_KHR_vulkan_memory_model`**, no `vkmm*` sync — it is not in the vendored headers here and it makes the correctness argument untestable headlessly.
- **Descriptor discipline**: one per-frame set (set 0, updated once per frame), one per-material set (set 1), push constants for the model matrix + material/texture indices. Query `maxPushConstantsSize` (guaranteed floor is 128 bytes) and `static_assert` our struct fits.
- **Shaders are SPIR-V shipped as bytes**, compiled offline (`glslangValidator`/`glslc`) and validated in CI (`spirv-val`). No runtime GLSL compilation path.
- **Swapchain is owned by the context**; on window resize recreate it (and recreate the framebuffers/pipelines that depend on extent), never leak the old one.
- **Subpass discipline inside one render pass**: declare dependencies, don't rely on implicit ordering; blending is done with explicit pipeline variants (opaque = blend disabled, transparent = blend enabled + sorted), which is how raylib's painter's-algorithm semantics map over.
- **Headless-testable by construction**: pure logic (math, handle bookkeeping, create-info assembly, batch flushing decisions, `VkResult`→`XudResult` mapping) lives in functions that take structs and never call the driver, so `tests/` can run with no device present.

## Design sketch

**Scope: 3D only** (settled 2026-10-23, see *Settled decisions*) — the sketch below is one perspective 3D scene: one
camera, one depth buffer, one render pass, one batch. There is no orthographic pass and no sprite batch anywhere in it.

Layers, bottom to top (each layer only sees the one below it):

```
GLFW window (extent, input, framebuffer-size)
   └─ Vulkan loader (dlopen libvulkan.so, PFN table)
       └─ instance (+advertised validation layers, debug-gated)
           └─ physical device (caps recorded: exts, features, limits)
               └─ device + queues (graphics+present, one queue family checked)
                   └─ swapchain (VkSurfaceKHR from GLFW, recreate on resize)
                       └─ xudrl renderer (passes, pipelines, batches, buffers)
                           └─ feature modules (shapes, text, textures, models, audio, camera)
                               └─ xud.h public API (raylib-shaped)
```

Frame loop (`BeginDrawing` → `EndDrawing`):

```
BeginDrawing: acquire image (semaphore) → begin cmd → begin render pass → clear color+depth
  draw calls: append each attribute into its own dynamic stream buffer, indices into the dynamic IB (SoA,
              settled 2026-10-23); flush on pipeline/material change
EndDrawing: end pass → end cmd → submit (graphics) → present (queue) → signal fence (per-image)
```

Pipeline set (created once, all GLSL→SPIR-V at build time):

| Axis | Variants | Why |
| --- | --- | --- |
| Shader | untextured-solid, textured, line, point/glyph | raylib's primitive families, **3D-only** — no ortho/sprite variant (scope settled 2026-10-23) |
| Blend | opaque (blend off, depth write) / transparent (blend on, depth test, sorted) | raylib draws in call order |

Descriptor layout:

| Slot | Contents | Updated |
| --- | --- | --- |
| set 0 | VP matrix, camera pos, time, fog, ambient, light count + fixed-size light array | once per frame, one `vkCmdCopyBuffer` from the staging ring |
| set 1 | combined image sampler array (partially bound, descriptor-indexed) | once per material change; textures suballocated in one atlas-friendly pool |
| push constants | model matrix (64 B) + material idx + texture idx + flags (≤128 B total) | per draw |

Vertex layout: **SoA** (settled 2026-10-23, see *Settled decisions*) — one stream per attribute, one `VkBuffer` per stream,
one `VkVertexInputBindingDescription` per stream (`stride` = that stream's element size, `offset` 0), one
`VkVertexInputAttributeDescription` per attribute pointing at its own binding. Today's four streams: `pos float3` (12 B/vertex)
+ `uv float2` (8) + `color ubyte4 normalized` (4) + `normal float3` (12) = 36 B/vertex **total**, split across four buffers
rather than interleaved into one. Same attribute set as raylib's `Vertex{pos, texcoords, color}` plus a normal so the default
material can light it.

GPU memory: one block allocator, ours, over the caps' recorded memory rows — `VkMemoryAllocateInfo` carries
`allocationSize` + `memoryTypeIndex` (one `VkDeviceMemory` per allocation, the index into the caps' `memoryTypes[]`),
`VkMemoryRequirements` is what a buffer/image asks for. Static geometry one-time, per-frame data from a ring, no
`vkAllocateMemory` inside a frame.

## Testing rules

- Every public function gets at least one test in `tests/`.
- Split pure logic from GPU code so the default test run passes **with no device present**.
- GPU tests are gated by `XUD_GPU_TESTS=1`; the default run must stay green on a machine with no driver.
- Tests must be deterministic — seed any RNG from a fixed test seed.
- A failing test blocks the commit. Run the test binary (and the `-fsanitizer` build) before committing — see the *Commit rule* below for the full bar.
- Validation-layer output is a test signal: a GPU test run in debug must be validation-clean, not just "didn't crash".

## Working rules

- Update `todo.md` when starting or finishing work: move the item, add a one-line note with the date. Once a step is done and committed, move the item **and** its long note out of `todo.md` into `todo_completed.md` (newest first) — `todo.md` stays the open-work tracker, the log stays complete, nothing finished is ever deleted.
- **Commit rule: commit as soon as the step is bug-free and stable — do not wait for the feature to be finished.** A step counts as *stable* only when all four hold:
  1. the step's item in `todo.md` is ticked or moved to the next section (or on to `todo_completed.md`), with a dated one-line note;
  2. `gcc -fsyntax-only -std=c11 -Wall -Wextra -Wpedantic -Werror` is clean and the test binary passes headless (plus `XUD_GPU_TESTS=1` runs where the step touches the driver, and the `-fsanitizer` build where it touches memory);
  3. no known bug, leak, or unimplemented path is being papered over — a workaround belongs in a code comment, never in a commit message;
  4. `AGENTS.md`, `todo.md`, `todo_completed.md` and the file-top comments describe exactly what is in the tree, nothing more and nothing less.
  Meet the bar, then commit immediately, even mid-subsystem. Never batch several steps into one commit at the end of a session, and never commit a state you are unsure about — if the bar is not met, keep working or write the blocker into `todo.md` instead.
- Commit messages: short imperative mood, ≤ 50 chars, no trailing period (`Add Vulkan instance`, `Fix swapchain resize`).
- No new dependencies without asking first — none of them are packaged here, so "add a dep" always means "vendor a header or a whole source tree". Settled so far: **Volk + GLFW vendored in** (`src/external/`, 2026-10-23, committed with their license files), **VMA settled out** (2026-10-23). Still open: miniaudio, cgltf, stb, …
- Ask before: changing the public API surface, adding a subsystem, or picking a Vulkan extension (the 2D/3D scope question is
  settled — **3D only**, see *Settled decisions*).
- Never commit `bin/`, `build/`, `reference/`, or third-party **reference** material. Never edit `reference/` (including `reference/raylib/`, which is gitignored — it still must not be edited even though it is untracked). The vendored material in `src/external/` is a **different bucket** — upstream code the library builds against, with its license files (`volk.h` MIT notice at the end of the file, `glfw/LICENSE.md`) — and **is committed with them** (decided 2026-10-23, recorded in `src/external/PROVENANCE.md`); never edit it in place either, re-vendor from upstream instead.

## Settled decisions (2026-10-16)

- **Language/standard: C11**, gcc 16.2.1 primary, clang 22.1.8 as a second opinion in CI.
- **Build: CMake + ninja**, plus a documented raw-`gcc` one-liner for the single-TU/stb-style include path.
- **Window/input backend: GLFW 3.5.1, vendored** (decided 2026-10-23; it was the system package until then). The sources live at `src/external/` (see the bullet below); the reason for GLFW is unchanged — 3.5 exposes `glfwCreateWindowSurface` + `glfwGetPhysicalDevicePresentationSupport` + `glfwGetRequiredInstanceExtensions`, so GLFW hands us the `VkSurfaceKHR` and the instance extensions it needs, and the swapchain is still ours. Vendoring is what makes the library build on a machine without the Arch `glfw` package (`local/glfw 1:3.5.1-1`) and keeps a `-lglfw` off the link line. **Consequence**: GLFW's platform backend becomes a build-time choice — the vendored tree carries `src/wl_*.c` (Wayland, with `deps/wayland/*.xml` that need `wayland-scanner`) and `src/x11_*.c` (X11), plus the halves that must be excluded from a Linux build (`win32_*`, `cocoa_*`/`macos_*`, `wgl_`/`glx_`/`egl_`/`osmesa_context.c`); the system `libglfw.so.3.5` links only `libc`/`libm` (probed with `ldd`) and names `libEGL.so.1`/`libGL.so.1`/`libGLX.so.0`/`libdecor-0.so.0` as strings, i.e. it `dlopen`s its backend at runtime, so a vendored build has to pick one explicitly. **The vendored tree is built in now** (2026-10-23, Wayland — see *Verified GLFW Wayland build*): 21 GLFW objects sit in the `xud` archive and the four Wayland libs are linked into every executable that links `xud`, with no `-lglfw` anywhere; **done since then (2026-10-23, S7)**: `src/xud/xud_window.h` includes `GLFW/glfw3.h` and the GPU test run creates a real window and destroys its surface — see *Verified window + surface facts*; there is still no X11 path of our own and no RGFW.
- **Vulkan access: `dlopen` the loader, no `-lvulkan` link dependency** — now through **vendored Volk** (decided 2026-10-23, see the bullet below).
- **Validation layers on in debug**, requested by enumerating advertised layers (never by name). `VK_VALIDATE_*` core features are unavailable here (SDK-only) — use `VkValidationFeaturesEXT` on the `pNext` chain and/or `DebugUtilsMessengerCreateInfoEXT` for the log side, and the `VK_INSTANCE_LAYERS` env as the user-facing override.
- **Synchronization: classic** (semaphores + per-image fences + pipeline barriers + explicit subpass deps). No `VK_KHR_vulkan_memory_model`.
- **Shaders: offline SPIR-V only** (`glslangValidator` at build time, `spirv-val` in CI). No runtime GLSL.
- **Minimum core version: `VK_API_VERSION_1_0` floor at instance creation + `VK_KHR_SWAPCHAIN`**, everything above that gated on recorded caps. Rationale: raylib's baseline portability feel; lavvk here reports 1.4.363 so the higher paths are exercised in tests, but the library must still work at the floor. The render-pass path is settled as explicit passes (2026-10-22, see *Settled decisions* below), so the floor stands as written and the dynamic-rendering path stays a future gate rather than an open question.
- **GPU memory: suballocated, one allocator, ring for transient** — the "no `vkCreateBuffer` per frame" rule above. The discipline stands either way; the allocator itself is settled to ours, see the bullet below.
- **Render pass: explicit `VkRenderPass` + `VkFramebuffer`** (decided 2026-10-22, facts above). Chosen over `VK_KHR_dynamic_rendering` (core in 1.3, extension below) and over gating both on caps, for four reasons: it is the only path that runs at the declared `VK_API_VERSION_1_0` + `VK_KHR_SWAPCHAIN` floor **without touching two landed contracts** (the dynamic feature is a *chained* struct — it would grow `XudDeviceCaps` by a 4th chained struct, restate the caps' `pNext` write-back check, and grow `VkDeviceCreateInfo.pNext`); it matches the design sketch's "begin render pass → clear color+depth" and the "subpass discipline inside one render pass" rule already here; it keeps the pure create-info assembly (`VkRenderPassCreateInfo`/`VkFramebufferCreateInfo`) the headless-test rule leans on; and it is the only path with a **resolve attachment**, which is not hypothetical — `framebuffer*SampleCounts` answer 0x1f here. Both gates are written down, not implemented: `VK_KHR_dynamic_rendering` (needs the caps/device `pNext` plumbing above) and `VK_KHR_create_renderpass2` as a strict upgrade of this path (`vkCmdBeginRenderPass2`/`vkCmdNextSubpass2`/`vkCmdEndRenderPass2`, +3 command-buffer slots, advertised here, extension-gated with the 1.0 form as fallback).
- **Scope: 3D only** (decided 2026-10-23). One perspective camera, one depth buffer, one render pass, one batch. The
  public API carries raylib's **3D half** of the shape surface — `DrawCube`, `DrawSphere`, `DrawMesh`, `DrawTriangle3D`,
  `DrawLine3D`, `DrawPoint3D` (names grepped in `reference/raylib/raylib.h`, lines 1585–1632) — and **not** its 2D half:
  no orthographic pass, no sprite batch, no screen-space `DrawRectangle`/`DrawCircle`/`DrawRectangleRec` family (lines
  1291–1306), no `BeginMode2D`/`EndMode2D` pair (2D at lines 1050–1051, the 3D pair at 1052–1053). Nothing is stubbed in their place — the names simply
  do not exist in `xud.h`. Rationale: it matches the Odin sibling; the opaque/transparent pipeline split, the depth-write
  vs depth-test pair and the sorted-transparent batch rule only mean anything with a depth buffer in play, so a 2D pass
  would be a second render pass + a second pipeline set + a second batch — a subsystem, not a flag; and raylib's 2D
  primitives are fixed-function screen quads that have no Vulkan shape here (repo rule: re-express it as a pipeline +
  batch, or drop it). Text/glyphs stay textured quads inside the one 3D pass (billboard or screen-aligned), which is why
  the point/glyph shader variant survives this decision; the text/font atlas itself stays open below.
- **GPU allocator: our own block allocator, no VMA** (decided 2026-10-23). One allocator owns all buffers/images; it
  suballocates over the memory rows the physical layer already recorded, and the per-frame ring is one of its regions.
  Header facts grepped in `/usr/include/vulkan/vulkan_core.h` (1.4.363), **no driver call**: `VkMemoryAllocateInfo` is
  **3783** — `{ sType, pNext, allocationSize, memoryTypeIndex }`, so one `VkDeviceMemory` per allocation and the type is
  an **index into the caps' `memoryTypes[]`**, not a mask; `VkMemoryRequirements` is **3790** — `{ size, alignment,
  memoryTypeBits }` — the bitmask of types a buffer/image may be bound to, and the allocate info picks **one index**
  that must be set in it; the stamp is `VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO = 5`
  (**212**); `PFN_vkAllocateMemory` **4551** returns `VkResult` (maps through `xud_result_from_vk`) and `PFN_vkFreeMemory`
  **4552** returns **`void`** — same teardown rule as `vkDestroyDevice`/`vkGetDeviceQueue`: zero state before the call,
  check after it. `VK_MAX_MEMORY_TYPES` 32U / `VK_MAX_MEMORY_HEAPS` 16U are the fixed bounds of the `memoryTypes[]`/
  `memoryHeaps[]` arrays inside the core `VkPhysicalDeviceMemoryProperties` the caps embed (already listed in the
  physical header-facts block above). **Naming trap**: grepping `VK_MEMORY_TYPE` across the whole `/usr/include/vulkan/` tree gives **zero
  hits** — a memory type is `VkMemoryType { VkMemoryPropertyFlags propertyFlags; uint32_t heapIndex; }` (**3521**), a
  property-flag/heap-index pair keyed by its **index**, so the caps row the S5.4 note calls "UNIFORM_BUFFER" is a label
  for a row, not a constant anyone can write. Why ours: VMA is not packaged here (see *Toolchain*), so "use VMA" means
  vendoring ~200 KB of third-party header, and it would hide the create-info assembly that the headless-test rule leans
  on behind an interface `tests/` cannot exercise with no device present — the free-list over `(memoryTypeIndex, size,
  alignment)` triples is exactly the pure half that stays testable headless. VMA's headline upgrades are ones we have
  not asked for and would each need their own gate: dedicated allocation is a **`pNext` chain entry** on
  `VkMemoryAllocateInfo` (`VkMemoryDedicatedAllocateInfo`, **5704**, aliased `…KHR` at 11294) — which would make the
  allocator's create-info chain non-empty, the same `pNext` discipline the caps' write-back check depends on — and its
  extension-name constant is **not in these headers at all** (grep `DEDICATED_MEMORY_EXTENSION_NAME` across
  `/usr/include/vulkan/`: zero hits), so it could not even be requested by name without hardcoding a string. Loader-slot
  cost of the choice: the allocator step adds **2** device-level slots (`vkAllocateMemory`, `vkFreeMemory`); `XUD_LOADER_SLOT_COUNT`
  stays **20** today, neither is added yet.
- **Loader + window backend vendored** (decided 2026-10-23): Volk and GLFW move into `src/external/` (raylib's own
  directory name for vendored deps) and are built from there, not from a system package.
  **Volk** = `src/external/volk.h` + `volk.c`, zeux/volk by Arseny Kapoulkine, MIT (notice at the end of `volk.h`),
  `VOLK_HEADER_VERSION 366`. It replaced the table we hand-maintained (S6.5, 2026-10-23): `volk.h` declares **842** `extern PFN_vkXxx vkXxx;`
  entries (counted here, e.g. `extern PFN_vkCreateInstance vkCreateInstance;` at 2102), so under `VK_NO_PROTOTYPES` every
  Vulkan entry point is a **data pointer** that `volkLoadInstance`/`volkLoadDevice` fill — a direct `vkCreateInstance(...)`
  call compiles and links against that pointer, which turns the "no Vulkan symbols linked" rule from a convention into a
  link-time fact. Bring-up surface: `volkInitialize` / `volkInitializeCustom(PFN_vkGetInstanceProcAddr)` / `volkFinalize` /
  `volkGetInstanceVersion` / `volkLoadInstance` / `volkLoadInstanceOnly` / `volkLoadDevice` / `volkGetLoadedInstance` /
  `volkGetLoadedDevice` / `volkLoadInstanceTable` / `volkLoadDeviceTable` (`VolkInstanceTable`/`VolkDeviceTable`). Its POSIX
  bring-up is `dlopen("libvulkan.so.1", RTLD_NOW|RTLD_LOCAL)` (volk.c 114, fallback `libvulkan.so` 116) — same loader our
  own probe used, `RTLD_LOCAL` rather than `RTLD_GLOBAL`. **Include-order trap** (verified): volk.h errors
  `To use volk, you need to define VK_NO_PROTOTYPES before including vulkan.h` when `VULKAN_H_` is already in (volk.h 17–18)
  and defines `VK_NO_PROTOTYPES` itself when it is not (25–26) — so in every TU volk.h comes **before** `<vulkan/vulkan.h>`,
  or the consumer defines the macro first. **Probed here**: `#include "volk.h"` + a `main` compiles clean with gcc **and**
  clang under `-std=c11 -Wall -Wextra -Wpedantic -Werror` even though Volk is 366 and this machine's Vulkan-Headers are 363
  (`vk_platform.h`, which volk.h includes by default, is present in Arch's headers). Volk here is loader-only — no `vk_mem_alloc`
  reference anywhere in it, so the "our own block allocator" decision above still stands.
  **GLFW** = the full upstream 3.5.1 tree at `src/external/glfw` (86 files, 2.2 MB: `include/GLFW/glfw3.h` + `glfw3native.h`,
  35 `.c` files under `src/`, `CMakeLists.txt`, `LICENSE.md`, `CONTRIBUTORS.md`, `deps/wayland/*.xml`); license notice is the
  3-point "provided as-is" text in `LICENSE.md` (Copyright © 2002-2006 Marcus Geelnard, 2006-2019 Camilla Löwy).
  **Done since then (2026-10-23, S6.5)**: `CMakeLists.txt` compiles `src/external/volk.c` into the `xud` archive (8
  objects, `volk.c.o` among them) and adds `src/external` to the include path; every Vulkan-bearing xud header now
  includes `volk.h` before `<vulkan/vulkan.h>`; and the loader rework landed as **a thin per-context wrapper over
  Volk's tables** — `XudLoader` keeps its name and its place in `XudContext`, but its contents are Volk's
  `struct VolkInstanceTable` + `struct VolkDeviceTable` plus three loader-level rows, and the per-name machinery
  (`XUD_LOADER_SLOT_COUNT`, `XUD_LOADER_NAMES`, `XudPfnBits`, `xud_loader_resolve`, the per-name `xud_loader_bind`,
  `resolvedCount`) is gone. The wrapper was kept, not dropped, because Volk's entry points are process-wide globals
  and the library must run more than one context. See *Verified Volk bring-up* above for the probed facts.
  **Done since then (2026-10-23, the same day)**: `CMakeLists.txt` now compiles the vendored GLFW tree as the OBJECT
  target `glfw_vendor` on the **Wayland** backend and puts its 21 objects into the `xud` archive (29 objects total),
  generating the 9 Wayland protocol header/code pairs into `build/glfw-wayland/` with `wayland-scanner` and linking the
  four Wayland libs PUBLIC into `xud` — see *Verified GLFW Wayland build* above for the source set, the warning-mode
  rule and the probed link. **Done since then (2026-10-23, the same day — S7)**: `src/xud/xud_window.h` includes
  `<GLFW/glfw3.h>` (after `<vulkan/vulkan.h>`, see *Verified window + surface facts*), the GPU test calls GLFW, and a
  window plus a `VkSurfaceKHR` exist in that run — the S7 item is complete and the swapchain below it is what is still
  open.
- **GLFW platform backend: Wayland** (decided 2026-10-23; X11 stays out). The vendored tree gates on `GLFW_BUILD_WAYLAND`
  (top-level `cmake_dependent_option`, ON for `UNIX;NOT APPLE`, glfw/CMakeLists.txt 30 — the old `GLFW_USE_WAYLAND` switch
  was removed and errors on a stale cache at line 24, so the option names are `GLFW_BUILD_WAYLAND`/`GLFW_BUILD_X11`) and
  compiles `-D_GLFW_WAYLAND` (`glfw/src/CMakeLists.txt` 54–55). It then **requires** `wayland-scanner` (`find_program`,
  `FATAL_ERROR` at 77 if missing) and generates the protocol headers/sources from the **9** XMLs GLFW ships in `deps/wayland/`
  (`wayland.xml`, `viewporter.xml`, `xdg-shell.xml`, `idle-inhibit-unstable-v1`, `pointer-constraints-unstable-v1`,
  `relative-pointer-unstable-v1`, `fractional-scale-v1`, `xdg-activation-v1`, `xdg-decoration-unstable-v1`, generated at
  100–107) and `pkg_check_modules(Wayland REQUIRED wayland-client>=0.2.7 wayland-cursor>=0.2.7 wayland-egl>=0.2.7
  xkbcommon>=0.5.0)` (155–160). **All of it is satisfied here** (probed 2026-10-23): `pkg-config --exists` answers yes for
  all four; `wayland-scanner` is at `/usr/bin` (`local/wayland 1.26.0-1`); the libs are `libwayland-client.so.0.26.0`,
  `libwayland-cursor.so.0`, `libwayland-egl.so.1`, `libxkbcommon.so.0.13.2`. `glfw/src/wl_platform.h` includes
  `<wayland-client-core.h>` + `<xkbcommon/xkbcommon.h>` + `<xkbcommon/xkbcommon-compose.h>` (27–29). `local/libdecor 0.2.5-1`
  is present but 3.5.1's CMake does **not** use it (grepping `LIBDECOR`/`decor` in `glfw/src/CMakeLists.txt` hits only the
  `xdg-decoration-unstable-v1.xml` protocol line). `GLFW_BUILD_X11` (29) stays OFF, so `x11_*.c` and `xkb_unicode.c` are
  excluded from the build — still no X11 path of our own and no RGFW.
  **Built in now (2026-10-23)**: `CMakeLists.txt` uses none of upstream's CMake — it **reads** upstream's
  `glfw/src/CMakeLists.txt` for `GLFW_BUILD_WAYLAND=ON` and compiles that source set itself as the OBJECT target
  `glfw_vendor`, with `-D_GLFW_WAYLAND` (upstream's own backend define; the option switch itself exists only in
  upstream's build, not in ours) plus `-D_DEFAULT_SOURCE`, `wayland-scanner` found by `find_program` with a
  `FATAL_ERROR` when it is missing (upstream's own rule), the 9 protocol pairs generated into `build/glfw-wayland/`,
  and `pkg_check_modules` for the four Wayland libs, whose link flags are attached PUBLIC to `xud` (an OBJECT library
  propagates none of its own). See *Verified GLFW Wayland build* above for the probed source set, warning mode and link.
- **Vendored third-party sources are committed, with their license files** (decided 2026-10-23). `src/external/` — Volk
  (MIT notice at the end of `volk.h`) and GLFW (zlib/libpng, `glfw/LICENSE.md`) — is tracked, next to
  `src/external/PROVENANCE.md`, which records versions, licenses and how the copies were made. This is the **opposite policy**
  from `reference/raylib/` (reference material: never committed, never edited). Consequence: the repo carries ~2.7 MB of
  upstream code, and a version bump is a **re-vendor commit**, never an in-place edit of those files.
- **Public API currency: index+generation slot handles, not raylib-style pointers** (decided 2026-10-23).
  **What raylib actually does** (re-read today in `reference/raylib/raylib.h`, 6.2): its public types are **value structs
  that embed driver names and own sub-objects through pointer arrays** — `Texture` (273–279) is `{ unsigned int id; int
  width, height, mipmaps, format; }` where `id` is an **OpenGL** texture name, `Texture2D` is just an alias of it (282);
  `Mesh` (ends 372) carries the CPU arrays (`float *vertices/texcoords/normals/tangents`, `unsigned char *colors`,
  `unsigned short *indices`) plus `unsigned int vaoId` and `unsigned int *vboId`; `Model` (ends 433) owns `Mesh *meshes`,
  `Material *materials`, `int *meshMaterial`, a `ModelSkeleton`, `Transform *` poses and `Matrix *boneMatrices`;
  `Font` (ends 324) owns a `Texture2D texture` **by value** plus `Rectangle *recs` and `GlyphInfo *glyphs`;
  `Material` (ends 392) owns `Shader shader` by value plus `MaterialMap *maps`; `Shader` (ends 378) is
  `{ unsigned int id; int *locs; }`; and `LoadMaterials` (1653) returns a raw `Material *` array. Ownership and lifetime
  are invisible to the caller, and the GL id fields have **no Vulkan counterpart** — that shape is a fixed-function-era
  artifact, exactly what the "no OpenGL, no state machine" rule refuses.
  **The decision**: the public API hands out one handle type — `{ uint32_t index; uint32_t generation; uint32_t kind; }` —
  validated against a **per-kind slot array** (textures, fonts, meshes, materials, models, and audio streams when audio
  lands), each record holding the Vulkan handles and the caps-gated variant info. Consequences, all of them load-bearing:
  creation functions **fill an out-param handle and return `XudResult`** (the house rule) and nothing returns a library-owned
  pointer; teardown is `xud_close_*`/`xudUnload*(handle)` → `XudResult`, there is no `Detach*`-returns-pointer form;
  handles are **value types** — copyable, storable in caller structs, comparable field-wise
  (`a.index == b.index && a.generation == b.generation`), and one kept across a `xud_close_*` is **rejected**, not dangling;
  the generation bumps on slot reuse, so a stale handle fails the equality check (the wrap case — 2³² reuses of one index —
  would alias, and that is documented, not silently ignored); and raylib's habit of hanging **CPU-side** source data off the
  public type (`Mesh.vertices`, `Font.recs`, `Model.meshes`) does **not** survive: CPU-side upload data becomes a separate
  caller-owned input struct (`xud_upload_mesh(...)` takes it), the handle addresses **only** the GPU-side object, and anything
  the caller wants back is read through accessors.
  **Terminology trap**: "Vulkan-style handle" is **not** Vulkan's own handle. In these headers `VK_DEFINE_HANDLE(object)` is
  `typedef struct object##_T* object;` (vulkan_core.h 26) — an opaque **pointer** —
  `VK_USE_64_BIT_PTR_DEFINES` defaults to 1 there (29–33) and `xud_loader.c` asserts `== 1` (line 38), so
  `VkImage`/`VkDevice`/`VkBuffer` are 64-bit pointers. Those stay **inside** the slot records,
  per the existing rule that callers never hold a `VkDevice`, `VkSwapchainKHR`, or raw `VkBuffer`; the public handle is the
  index+generation pair. **Not in the tree yet** (until S10): `xud.h` today exposes only `XudContext**` + `XudResult` (no object handle
  exists), and no slot array exists — the handle type and its slot arrays land with the first GPU-object API (textures/fonts/models),
  and the pure half (slot allocation, generation bump, stale-handle rejection) is headless-testable by construction. **In the tree
  since 2026-10-24** (S10): `xud.h` declares `XudHandle` + the `XUD_HANDLE_KIND_*` values, and `src/xud/xud_handle.h` owns the per-kind
  slot arrays — but a slot's `record` field is still the NULL sentinel of the allocator/renderer rung, so no handle names an object yet
  and `xud.h` still declares no create/close function that takes one (see *Verified handle layer facts (2026-10-24)*).
  **The handle carries a kind tag** (decided the same day): `typedef struct XudHandle { uint32_t index; uint32_t generation;
  uint32_t kind; } XudHandle;` — 12 bytes, no padding, `static_assert(sizeof(XudHandle) == 12u, "...")` (message-carrying form
  required: clang 22 rejects a message-less `static_assert` under these flags, see *Verified loader bring-up*). There is **one**
  handle type for every object kind, because the tag is what distinguishes kinds — the compiler can no longer catch a cross-kind
  mix-up (all kinds share the type), the runtime check does, exactly and cheaply. Validation at every public entry point is the
  three-part test in that order — `index < slots[kind].count`, then `generation` equality, then `kind` equality — and any failure
  answers `XUD_INVALID_ARG`, the existing `XudResult` code for a bad argument (`xud.h` 83–91). The tag is **not** redundant with
  the array identity: it is what makes the check exact, since a texture handle's `(index, generation)` pair could otherwise match
  a live model slot by coincidence. `XUD_HANDLE_KIND_NONE = 0u` is never stored in a live slot, so a **zero-initialised** handle —
  the state a caller gets when it ignores the `XudResult` of the create call — is rejected by the tag compare before any array
  lookup happens. Kind values are `XUD_HANDLE_KIND_TEXTURE`/`FONT`/`MESH`/`MATERIAL`/`MODEL` (+ `AUDIOSTREAM` when audio lands,
  extended whenever a new caller-addressable kind lands); the library-owned objects callers never name (batch VB/IB, per-frame
  ring, swapchain images, pipelines, descriptor sets) stay tag-less. Constant naming follows the layer convention already in the
  tree (`XUD_DEVICE_QUEUE_MAX_COUNT 2u` at `xud_device.h` 92, `XUD_DEVICE_MAX_DEVICE_EXTENSIONS 8u` at 110, both guarded by
  message-carrying `static_assert`s at 128/131).
- **Data layout: SoA, no ECS at this level** (decided 2026-10-23). **SoA** for the attribute streams — one stream per
  vertex attribute, one `VkBuffer` per stream, one `VkVertexInputBindingDescription` per stream (`stride` = that stream's
  element size, `offset` 0) and one `VkVertexInputAttributeDescription` pointing at its own `binding`. Today's four streams:
  `pos` `float3` (12 B/vertex, `VK_FORMAT_R32G32B32_SFLOAT`), `uv` `float2` (8 B, `VK_FORMAT_R32G32_SFLOAT`), `color`
  normalized `ubyte4` (4 B, `VK_FORMAT_R8G8B8A8_UNORM`), `normal` `float3` (12 B, `VK_FORMAT_R32G32B32_SFLOAT`) — the same
  36 B/vertex total as the sketch's interleaved form, split across four buffers instead of woven into one.
  **Header facts** (grepped today in `/usr/include/vulkan/vulkan_core.h`, 1.4.363, **no driver call**): `VkVertexInputAttributeDescription`
  **4267** = `{ location, binding, format, offset }` and `VkVertexInputBindingDescription` **4274** = `{ binding, stride,
  inputRate }` — **neither is a stamped struct** (no `sType`, no `pNext`, unlike every create-info this tree assembles, so
  the "stamp the whole array" rule that the physical layer leans on has nothing to stamp here); `VkPipelineVertexInputStateCreateInfo`
  **4377** carries both arrays (`vertexBindingDescriptionCount`/`pVertexBindingDescriptions`, `vertexAttributeDescriptionCount`/`pVertexAttributeDescriptions`).
  `inputRate` is `VK_VERTEX_INPUT_RATE_VERTEX` (2658) / `VK_VERTEX_INPUT_RATE_INSTANCE` (2659) — a future instancing path is one
  more per-instance binding on the same buffers, which is part of why the stream form survives and a fixed-size interleaved
  vertex does not. `maxVertexInputAttributes` (**3614**) and `maxVertexInputBindings` (**3615**) sit inside `VkPhysicalDeviceLimits`
  (**3584**), which the caps already embed whole, so the binding count is gated on the caps and never assumed. **Naming trap**:
  the attribute's `format` field is a plain **`VkFormat`** here — `VkVertexFormatType` / `VK_VERTEX_FORMAT_*` are **absent from
  these headers** (grep `VERTEX_FORMAT` across `/usr/include/vulkan/`: **zero hits**), so streams are spelled with format names,
  and `VK_FORMAT_R8G8A8_UNORM` **does not exist** (grep `R8G8A8` across `vulkan_core.h`: **zero hits**) — the 4-byte color
  stream is `VK_FORMAT_R8G8B8A8_UNORM` (37), byte order R,G,B,A. One `vkCmdBindVertexBuffers` call binds the whole array
  (`PFN_vkCmdBindVertexBuffers` **4655**, `(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets)`) and returns
  **void** — the same zero-state-before-the-call rule `vkDestroyDevice`/`vkGetDeviceQueue`/`vkFreeMemory` follow.
  **Why SoA**: raylib's own `Mesh` (`reference/raylib/raylib.h` **346–372**) is already SoA on the CPU side — one array per
  attribute (`float *vertices/texcoords/texcoords2/normals/tangents`, `unsigned char *colors`, `unsigned short *indices`) and
  `unsigned int *vboId` (371, "Vertex Buffer Objects id (default vertex data)", **plural**: one VBO per attribute) — so the
  caller-owned upload struct the handle-currency decision already fixes (`xud_upload_mesh(...)` takes it) arrives in the form
  the streams want: no de-interleave step, one staging copy per stream. Consequences, all load-bearing: the dynamic batch becomes
  **N stream buffers + one index buffer** (the allocator's suballocation handles the count, no new mechanism), the batch's
  pure-half invariant is "every stream holds the same vertex count" — headless-testable by construction — a pipeline/material
  change flushes per stream, and an attribute set can grow (tangents, bone indices/weights, the second uv set raylib's `Mesh`
  carries) without re-laying-out a fixed-size vertex: each new attribute is one more binding gated on the caps'
  `maxVertexInputBindings`. **Driver facts, probed today** (scratch program linked against `build/libxud.a`, loader → instance
  → physical, deleted after running; identical with the validation layer on and off): `maxVertexInputAttributes` **29**,
  `maxVertexInputBindings` **33**, `maxVertexInputAttributeOffset` 2047, `maxVertexInputBindingStride` 4095, `maxPushConstantsSize`
  **256**. Those **do not match** the numbers the S5.4 driver note records (128 push constants, 32 attributes, `deviceID` 0x400,
  name "Intel Iris Graphics … (TigerLake-LP GT2 …)") while `vendorID` 32902 = 0x8086 and `apiVersion` 4211042 do match — today's
  device says `deviceID` 39497 and "Intel(R) Iris(R) Xe Graphics (TGL GT2)", i.e. the machine's Vulkan driver moved since
  2026-10-18, so the limits a step reads come **from the caps**, never from that note's numbers. Headroom here: 33 bindings
  against today's 4, so the attribute set can grow without a driver refusal. **No ECS at this level**: the CPU-side records stay
  one plain struct per object kind inside its slot (`XudMeshRecord`, `XudTextureRecord`, … — illustrative names, none of
  them in the tree yet), not an archetype/component registry — the per-kind slot arrays the handle-currency decision
  already fixes are the only arrays the library owns and are what an iteration-friendly pass would walk. An ECS storage
  layer in C11 would be a subsystem (sparse sets, archetype storage, system registration, storage migration) the raylib-shaped
  public API cannot express and no test here would exercise, and it buys nothing the SoA streams have not already bought.
  **Not in the tree yet**: no vertex stream, no batch, no `VkPipelineVertexInputStateCreateInfo` — this is a decision the
  batch/shapes steps write against, not code.

## Open design decisions (ask, don't assume)

- **Model format**: glTF 2.0 via vendored cgltf (raylib does exactly this) vs a minimal `.glb` reader of our own.
- **Audio backend**: vendor miniaudio (raylib's `raudio` does) vs no audio yet.
- **Text/font**: bitmap-font atlas of our own vs vendored `stb_truetype`/fontstash.
