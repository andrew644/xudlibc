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
| `shaders/` | GLSL sources + built `.spv` | Compiled at build time, never at runtime |
| `third_party/` | Vendored single-header deps | Only with an explicit decision (see Open decisions) |
| `reference/raylib/` | raylib **6.2** headers (present today, 560 KB) | **NEVER EDIT** — gitignored, never `#include`d, never built against |
| `bin/`, `build/` | Build output | Gitignored, never committed |
| `todo.md` | Project progress tracker (open work only) | Update every session, dated one-liners |
| `todo_completed.md` | Finished-step log, newest first | Where completed items and their long notes move to — never delete a finished step |

**Present today (after the diagnostics + instance + physical + device steps)**: `src/xud/` = `xud.h` + `xud_core.h`/`xud_core.c` (context shell + teardown) + `xud_loader.h`/`xud_loader.c` (the `dlopen` table) + `xud_vulkan.h`/`xud_vulkan.c` (shared vocabulary: `VkResult`→`XudResult` funnel + fixed-buffer name helpers, no driver call) + `xud_diagnostics.h`/`xud_diagnostics.c` (the diagnostics layer: advertised layer selection, `VkValidationFeaturesEXT`, the `VK_EXT_debug_utils` messenger + log callback) + `xud_instance.h`/`xud_instance.c` (the `VkInstance` layer: application info, extension selection, create-info assembly, create/destroy) + `xud_physical.h`/`xud_physical.c` (the physical layer: `XudPhysicalDevice` + an embedded `XudDeviceCaps` + the layer's contract, **both halves implemented** — the pure half (selection, feature intersection, the graphics queue-family rule, `apiVersion` read-back, free-only teardown) landed at S5.3, the driver half `xud_physical_query_devices`/`xud_physical_open` at S5.4, and this `.c` is in the archive with them) + `xud_device.h`/`xud_device.c` (the device layer: `XudDevice` owning the `VkDevice` handle, the `VkDeviceQueueCreateInfo` row the driver was handed, the `VkPhysicalDeviceFeatures` it was handed, the enabled-extension names it was handed, and the two role queues it resolved; `XudQueue` is a **record** of a queue, not an owning type — there is no `vkDestroyQueue`; one queue row from the physical layer's chosen family, `VkDeviceCreateInfo` carrying the physical layer's **feature intersection** and only advertised device extensions, the `xud_device_wait_idle` guard, `xud_device_close` = guard → destroy → free, **both halves implemented** — the pure half (row assembly, extension selection, create-info assembly, queue lookup) and the driver half (`xud_device_open`/`xud_device_wait_idle`/`xud_device_close`), and this `.c` is in the archive with them); `tests/` = `xud_test.c`; `examples/` = `hello_context.c`; `reference/raylib/`; `build/` (gitignored CMake output, plus `build-san/` when sanitizers are wanted). **Not created yet**: `shaders/`, `third_party/`, `bin/`. A `VkPhysicalDevice` **and** a `VkDevice` with its queues **are** created now — the device layer is implemented, in the archive, and tested against the driver — but **nothing below the physical layer is wired into the `XudContext` yet**: the context's device/queue/surface/swapchain/allocator fields stay sentinels until S8's ownership step, and there is still no surface, swapchain, or allocator.

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
| Window/input | system **GLFW 3.5.1** (`pkg-config glfw3`, `/usr/include/GLFW/glfw3.h`, `libglfw.so.3.5`) |
| Shader tools | `glslangValidator` (glslang 11:16.4.0, GLSL 4.60 / ESSL 3.20), `glslc` 2026.3, `spirv-val`, `spirv-opt`, `spirv-as` |
| **Not installed** | No Vulkan SDK, no Volk, no VMA, no miniaudio, no cgltf, no stb, no `cglm` — anything like that must be vendored by hand, and that needs a decision first |

**Verified GLFW 3.5.1 Vulkan surface**:
`glfwVulkanSupported()`, `glfwGetRequiredInstanceExtensions(uint32_t *count)`, `glfwGetInstanceProcAddress(VkInstance, const char *)`,
`glfwGetPhysicalDevicePresentationSupport(VkInstance, VkPhysicalDevice, uint32_t queuefamily)`,
`glfwCreateWindowSurface(VkInstance, GLFWwindow *, const VkAllocationCallbacks *, VkSurfaceKHR *)`, `glfwInitVulkanLoader(PFN_vkGetInstanceProcAddr)`.
There is **no** `glfwDestroyVulkanSurface` in 3.5 — destroy the surface with `vkDestroySurfaceKHR`.

**Verified loader bring-up (2026-10-16)** — the shape `src/xud/xud_loader.c` uses:
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

**Verified header gap:** `VkPhysicalDeviceValidationFeatures` / `VK_VALIDATE_*` are **not in Vulkan-Headers at all** (SDK-only). The only validation-feature struct available here is `VkValidationFeaturesEXT` (a `pNext` chain on `VkInstanceCreateInfo`), plus `VK_VALIDATION_FEATURE_ENABLE_*` / `VK_VALIDATION_FEATURE_DISABLE_*` enums from `vulkan_ext` headers — `xud_diagnostics.c` asks for `VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT` and `VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`. Confirm with a grep before using any of these names.

## Commands

| Task | Command |
| --- | --- |
| Configure | `cmake -B build -G Ninja` (capital **N** — cmake 4.4.3 rejects `-G ninja`) |
| Build everything | `ninja -C build` |
| Build one example | `ninja -C build <target>` |
| Syntax/lint check (fast, no codegen) | `gcc -fsyntax-only -std=c11 -Wall -Wextra -Wpedantic -Werror -I src/xud src/xud/*.c` |
| Unit tests (headless) | `./build/xud_test` |
| GPU tests | `XUD_GPU_TESTS=1 ./build/xud_test` |
| Sanitizer build | `cmake -B build-san -G Ninja -DXUD_SANITIZERS=ON` then `./build-san/xud_test` (adds `-fsanitize=address,undefined`) |
| Compile a shader | `glslangValidator -V -o shaders/solid.spv shaders/solid.vert` (and `-G` for geometry/comp) |
| Validate SPIR-V | `spirv-val shaders/solid.spv` |

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
- Opaque handles = `{ uint32_t index; uint32_t generation; }` validated against a slot array. Stale handles must be caught, not UB.
- Cleanup idiom: `goto fail;` ladder in init paths, one `xud_close_*()` teardown; every partial-failure path must leak nothing.
- No global mutable state except the single library context pointer; multiple windows must work.
- `#include` guard `XUD_<FILE>_H_`; file-top comment stating what the file owns and which layer it belongs to.
- All blocks must have curly braces. Even one line if statements.
- Use clang-format before commit.
- Doc comments: `///` Doxygen-style above public declarations only (no Doxygen build required yet).
- Compile flags in dev: `-std=c11 -Wall -Wextra -Wpedantic -Werror` plus `-fsanitize=address,undefined` for tests. Release: `-O2`. The CMakeLists adds these itself (`-Wall -Wextra -Wpedantic -Werror` always, `-g -DXUD_DEBUG=1` by default, `-O2` under `-DXUD_DEBUG=OFF`, sanitizers under `-DXUD_SANITIZERS=ON`).
- **Never add `-Ddefault=lib`** to a compile line here: with gcc 16.2.1 and clang 22.1.8 it is a preprocessor define of a macro named `default`, which rewrites every `default:` label in every `switch` and breaks the build (verified: both compilers fail a two-case switch under it).
- Under these flags, `dlsym` → function-pointer casts and message-less `static_assert` are **errors, not warnings** — see *Verified loader bring-up* for the union+`memcpy` idiom and the `static_assert(cond, "why")` form.

## Vulkan rules (best practices — these are the point of the repo)

- **No Vulkan symbols linked at build time.** Load `libvulkan.so` with `dlopen` + `vkGetInstanceProcAddr` (Volk-style), resolve entry points into a small internal function table. Rationale: the library must build and run on a machine with no driver, and `xud_vulkan_supported()` must be a real runtime query. Do **not** add `-lvulkan` to the link line.
- **Never hardcode a layer name.** Enumerate with `vkEnumerateInstanceLayerProperties`, request everything advertised, and re-create the instance with `ppEnabledLayerNames`. Validation is gated: `XUD_VALIDATE` on in debug builds, off in release, always overridable by env (`VK_INSTANCE_LAYERS`) or config.
- **Check before you use.** Every extension and feature is queried (`vkEnumerateDeviceExtensionProperties`, `vkGetPhysicalDeviceFeatures2`) and recorded in a `XudDeviceCaps` struct; `VkDeviceCreateInfo` carries **only** what was verified. Anything unverified stays off and the feature that needed it returns `XUD_UNSUPPORTED`.
- **Explicit pipelines, created once at init.** A small fixed set of variants (see Design sketch), never created per draw, never rebuilt per frame.
- **Allocate once, suballocate always.** One GPU-memory allocator owns all buffers/images; static geometry is a one-time allocation, per-frame data comes from a ring/transient allocation. **No `vkCreateBuffer` inside a frame.**
- **Staging via transfer ops only**: `vkCmdCopyBuffer` / `vkCmdCopyImage` / `vkCmdCopyBufferToImage` from a staging ring. No `vkMapMemory` per frame without a matching `vkUnmapMemory` + `VkBufferInvalidateRangeInfo` (or a `vkCmdPipelineBarrier` with the right scope), and prefer the copy path over mapped writes for anything transient.
- **Synchronization is classic**: acquire/present semaphores for frame boundaries, one `VkFence` per swapchain image, `vkCmdPipelineBarrier` and explicit subpass dependencies inside a command buffer. **No `VK_KHR_vulkan_memory_model`**, no `vkmm*` sync — it is not in the vendored headers here and it makes the correctness argument untestable headlessly.
- **Descriptor discipline**: one per-frame set (set 0, updated once per frame), one per-material set (set 1), push constants for the model matrix + material/texture indices. Query `maxPushConstantsSize` (guaranteed floor is 128 bytes) and `static_assert` our struct fits.
- **Shaders are SPIR-V shipped as bytes**, compiled offline (`glslangValidator`/`glslc`) and validated in CI (`spirv-val`). No runtime GLSL compilation path.
- **Swapchain is owned by the context**; on window resize recreate it (and recreate the framebuffers/pipelines that depend on extent), never leak the old one.
- **Subpass discipline inside one render pass**: declare dependencies, don't rely on implicit ordering; blending is done with explicit pipeline variants (opaque = blend disabled, transparent = blend enabled + sorted), which is how raylib's painter's-algorithm semantics map over.
- **Headless-testable by construction**: pure logic (math, handle bookkeeping, create-info assembly, batch flushing decisions, `VkResult`→`XudResult` mapping) lives in functions that take structs and never call the driver, so `tests/` can run with no device present.

## Design sketch

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
  draw calls: append vertices/indices into the dynamic VB/IB; flush on pipeline/material change
EndDrawing: end pass → end cmd → submit (graphics) → present (queue) → signal fence (per-image)
```

Pipeline set (created once, all GLSL→SPIR-V at build time):

| Axis | Variants | Why |
| --- | --- | --- |
| Shader | untextured-solid, textured, line, point/glyph | raylib's four primitive families |
| Blend | opaque (blend off, depth write) / transparent (blend on, depth test, sorted) | raylib draws in call order |

Descriptor layout:

| Slot | Contents | Updated |
| --- | --- | --- |
| set 0 | VP matrix, camera pos, time, fog, ambient, light count + fixed-size light array | once per frame, one `vkCmdCopyBuffer` from the staging ring |
| set 1 | combined image sampler array (partially bound, descriptor-indexed) | once per material change; textures suballocated in one atlas-friendly pool |
| push constants | model matrix (64 B) + material idx + texture idx + flags (≤128 B total) | per draw |

Vertex format (36 B, aligned): `pos float3` (12) + `uv float2` (8) + `color ubyte4 normalized` (4) + `normal float3` (12).
Matches raylib's `Vertex{pos, texcoords, color}` plus a normal so the default material can light it.

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
- No new dependencies (Volk, VMA, miniaudio, cgltf, stb, …) without asking first — none of them are packaged here, so "add a dep" always means "vendor a header".
- Ask before: changing the public API surface, adding a subsystem, picking a Vulkan extension, or choosing a 2D/3D scope.
- Never commit `bin/`, `build/`, `reference/`, or third-party material. Never edit `reference/` (including `reference/raylib/`, which is gitignored — it still must not be edited even though it is untracked).

## Settled decisions (2026-10-16)

- **Language/standard: C11**, gcc 16.2.1 primary, clang 22.1.8 as a second opinion in CI.
- **Build: CMake + ninja**, plus a documented raw-`gcc` one-liner for the single-TU/stb-style include path.
- **Window/input backend: system GLFW 3.5.1** (`pkg-config glfw3`). Chosen because 3.5 exposes `glfwCreateWindowSurface` + `glfwGetPhysicalDevicePresentationSupport` + `glfwGetRequiredInstanceExtensions`, so GLFW hands us the `VkSurfaceKHR` and the instance extensions it needs; the swapchain itself is still ours. No X11/Wayland path, no RGFW.
- **Vulkan access: `dlopen` the loader, no `-lvulkan` link dependency** (Volk-style, hand-rolled until Volk is explicitly approved for vendoring).
- **Validation layers on in debug**, requested by enumerating advertised layers (never by name). `VK_VALIDATE_*` core features are unavailable here (SDK-only) — use `VkValidationFeaturesEXT` on the `pNext` chain and/or `DebugUtilsMessengerCreateInfoEXT` for the log side, and the `VK_INSTANCE_LAYERS` env as the user-facing override.
- **Synchronization: classic** (semaphores + per-image fences + pipeline barriers + explicit subpass deps). No `VK_KHR_vulkan_memory_model`.
- **Shaders: offline SPIR-V only** (`glslangValidator` at build time, `spirv-val` in CI). No runtime GLSL.
- **Minimum core version: `VK_API_VERSION_1_0` floor at instance creation + `VK_KHR_SWAPCHAIN`**, everything above that gated on recorded caps. Rationale: raylib's baseline portability feel; lavvk here reports 1.4.357 so the higher paths are exercised in tests, but the library must still work at the floor. Revisit if the dynamic-rendering path wins (see Open decisions).
- **GPU memory: suballocated, one allocator, ring for transient** — the "no `vkCreateBuffer` per frame" rule above. Whether the allocator is VMA (vendored) or ours is still open; the discipline is not.

## Open design decisions (ask, don't assume)

- **2D vs 3D scope** — raylib has both; the Odin sibling is 3D-only. Which one does `xudlibc` ship? (Decides whether there is an ortho 2D pass and a sprite batch.)
- **Render pass style**: explicit `VkRenderPass` + `VkFramebuffer` (works at the 1.0 floor) vs `VK_KHR_dynamic_rendering` (core in 1.3, extension below) — or both paths gated on caps.
- **GPU allocator**: vendor VMA vs write our own block allocator.
- **Loader**: hand-rolled `dlopen` table vs vendor Volk (single header, MIT).
- **Model format**: glTF 2.0 via vendored cgltf (raylib does exactly this) vs a minimal `.glb` reader of our own.
- **Audio backend**: vendor miniaudio (raylib's `raudio` does) vs no audio yet.
- **Text/font**: bitmap-font atlas of our own vs vendored `stb_truetype`/fontstash.
- **Handles vs pointers** for the public API (raylib hands out `Texture*`-style pointers; Vulkan wants index+generation handles).
- **Data layout**: AoS vertex (sketched above) vs SoA/GPU-driven; ECS or plain structs.
