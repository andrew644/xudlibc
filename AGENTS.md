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
| `tests/` | Unit tests — **split 2026-10-25 into one TU per area** (`xud_test_harness.h`/`.c` = the shared counters, the one named check and the GPU gate; `xud_test_main.c` = the runner; `xud_test_<area>.c` × 19) | Headless by default; GPU tests gated by `XUD_GPU_TESTS=1`; the runner's area order is explicit in `CMakeLists.txt`, not globbed |
| `examples/` | Runnable demos | Small, one concern each |
| `shaders/` | GLSL sources (`solid.vert`/`solid.frag` present today) | Compiled at build time by CMake into `build/gen/` — never at runtime, never committed as `.spv` |
| `src/external/` | Vendored third-party deps (raylib's own convention) — **present today**: `volk.h`/`volk.c` (Volk) + a full `glfw/` source tree (GLFW 3.5.1) + `PROVENANCE.md` | Settled 2026-10-23 (see *Settled decisions*): **committed with its license files**, never edited in place; **Volk is built in now** (`volk.c` compiled into the `xud` archive, `src/external` on the include path), and so is the **GLFW tree** (the OBJECT target `glfw_vendor`, **Wayland** backend: 21 GLFW objects in the archive, 40 objects total, the four Wayland libs linked PUBLIC into `xud` — see *Verified GLFW Wayland build*) |
| `reference/raylib/` | raylib **6.2** headers (present today, 560 KB) | **NEVER EDIT** — gitignored, never `#include`d, never built against |
| `bin/`, `build/` | Build output | Gitignored, never committed |
| `todo.md` | Project progress tracker (open work only) | Update every session, dated one-liners |
| `todo_completed.md` | Finished-step log, newest first | Where completed items and their long notes move to — never delete a finished step |

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
| Window/input | **GLFW 3.5.1, vendored and built in** (`src/external/glfw`, Wayland backend — see *Verified GLFW Wayland build* and *Verified window + surface facts*): 21 GLFW objects sit in the `xud` archive, the four Wayland libs (`wayland-client`/`wayland-cursor`/`wayland-egl`/`xkbcommon`, all present here) are linked PUBLIC into every executable that links `xud`, and there is still **no `-lglfw`** anywhere. The system package (`local/glfw 1:3.5.1-1`, `/usr/include/GLFW/glfw3.h`, `libglfw.so.3.5`) is no longer what the build uses; **`src/xud/xud_window.h` includes `<GLFW/glfw3.h>` now** (after `<vulkan/vulkan.h>` — see the include-order rule in *Verified window + surface facts*), and the GPU test run creates a real window on this machine's Wayland session (`XDG_SESSION_TYPE=wayland`, `WAYLAND_DISPLAY=wayland-1`), so a window and a surface exist there — the headless run still creates neither; the renderer area's GPU half resizes a real window and recreates the swapchain at the framebuffer extent that follows it |
| Wayland (GLFW's backend) | `wayland-scanner` at `/usr/bin` (`local/wayland 1.26.0-1`); `pkg-config` finds `wayland-client`/`wayland-cursor`/`wayland-egl`/`xkbcommon` — the four GLFW 3.5.1 `pkg_check_modules` requires — and `libdecor-0` (`local/libdecor 0.2.5-1`, **unused** by 3.5.1's CMake); libs `libwayland-client.so.0.26.0`, `libwayland-cursor.so.0`, `libwayland-egl.so.1`, `libxkbcommon.so.0.13.2` |
| Shader tools | `glslangValidator` (glslang 11:16.4.0, GLSL 4.60 / ESSL 3.20), `glslc` 2026.3, `spirv-val`, `spirv-opt`, `spirv-as` |
| **Not installed** | No Vulkan SDK, no Volk, no VMA, no miniaudio, no cgltf, no stb, no `cglm` — anything like that must be vendored by hand, and that needs a decision first |

## Commands

| Task | Command |
| --- | --- |
| Configure | `cmake -B build -G Ninja` (capital **N** — cmake 4.4.3 rejects `-G ninja`) |
| Build everything | `ninja -C build` |
| Build one example | `ninja -C build <target>` |
| Syntax/lint check (fast, no codegen) | `gcc -fsyntax-only -std=c11 -Wall -Wextra -Wpedantic -Werror -I src/xud -I src/external -I build/gen src/xud/*.c` (**`-I src/external` is required** — every Vulkan-bearing xud header includes `"volk.h"` before `<vulkan/vulkan.h>`, and `volk.h` lives in `src/external/`; **`-I build/gen` is required** from S16 on — `xud_pipeline.c` includes `xud_solid_spv.h`, which only exists as a build-time generated header, so without it the check answers `fatal error: xud_solid_spv.h: No such file or directory` on the one file and nothing else) |
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
- **Explicit pipelines, created once at init.** A small fixed set of variants (see Design sketch), never created per draw, never rebuilt per frame. Today: the **two-row list** the batch's `variant` indexes (`pipeline[XUD_BATCH_VARIANT_COUNT]`, one layout for the whole list) — growing the list is a row, never a rename of the pair, and the frame loop **binds** the row a draw's variant names beside that draw.
- **Allocate once, suballocate always — with one exception, and the exception is a Vulkan fact.** One GPU-memory allocator owns all buffers/images; static geometry is a one-time allocation, per-frame data comes from a ring/transient allocation. **No `vkCreateBuffer` inside a frame.** The allocator is **ours** — no VMA, settled 2026-10-23, see *Settled decisions*. **Settled 2026-10-26**: the batch's staging arena is **not** ring memory — the ring is a cursor that wraps, so a ring carve is a batch whose bytes a later lap overwrites mid-frame; the arena is carved from the blocks, mapped once at open and unmapped once at close. **Settled 2026-10-26 (same day, the exception)**: `vkMapMemory` maps a `VkDeviceMemory`, not a range, so a suballocated carve inside a block someone else already mapped cannot be mapped at all (`VUID-vkMapMemory-memory-00678`, and lavvk answers it as `VK_ERROR_MEMORY_MAP_FAILED` rather than forgiving it). The one carve a rung maps therefore owns the **whole block**, which is why the allocator has two acquires — `xud_allocator_acquire()` (suballocating, free-list) and `xud_allocator_acquire_host_visible()` (whole block, no free-list scan, never split). Suballocation is for the bytes a frame reads through a transfer; a host mapping is a whole-memory decision. Two arenas spend it today: the batch's and the descriptor's staging arena for set 0's block. The ring is still unused by anything outside the allocator's own test — the per-frame uniform copy landed and is **not** what wanted it, for the batch's own reason above.
- **Staging via transfer ops only**: `vkCmdCopyBuffer` / `vkCmdCopyImage` / `vkCmdCopyBufferToImage` from a staging ring. No `vkMapMemory` per frame without a matching `vkUnmapMemory` + `VkBufferInvalidateRangeInfo` (or a `vkCmdPipelineBarrier` with the right scope), and prefer the copy path over mapped writes for anything transient.
- **Synchronization is classic**: acquire/present semaphores for frame boundaries, one `VkFence` per swapchain image, `vkCmdPipelineBarrier` and explicit subpass dependencies inside a command buffer. **No `VK_KHR_vulkan_memory_model`**, no `vkmm*` sync — it is not in the vendored headers here and it makes the correctness argument untestable headlessly.
- **Descriptor discipline**: one per-frame set (set 0, updated once per frame), one per-material set (set 1), push constants for the model matrix + material/texture indices. Query `maxPushConstantsSize` (guaranteed floor is 128 bytes) and `static_assert` our struct fits.
- **Shaders are SPIR-V shipped as bytes**, compiled offline (`glslangValidator`/`glslc`) and validated in CI (`spirv-val`). No runtime GLSL compilation path.
- **Swapchain is owned by the context**; on window resize recreate it (and recreate the framebuffers/pipelines that depend on extent), never leak the old one.
- **The swapchain's two out-of-band answer codes are a frame, not a refusal.** `vkAcquireNextImageKHR` and `vkQueuePresentKHR` are the only two entry points in this tree whose `VkResult` answers about the **swapchain** rather than about the call: `VK_SUBOPTIMAL_KHR` ("it works, it is just not the most optimal one for this surface") and `VK_ERROR_OUT_OF_DATE_KHR` ("it no longer matches the surface — create a new one **before the next acquire**, the image this call handed back is still valid and presentable"). For those two the image is drawn, submitted and presented as usual and the frame loop leaves `XudFrame.recreateWanted` set for the resize step to act on — this rung cannot recreate, the swapchain is the context's and every image, framebuffer row and fence index dies with it. That is why there are **two funnels** in `xud_vulkan.h`: `xud_swapchain_result_from_vk()` for those two calls and the general `xud_result_from_vk()` for everything else, which still maps both codes to `XUD_VULKAN_ERROR` because outside those two a positive code IS a driver fault. One table with exceptions would make the exception a caller's judgement rather than a fact of the entry point. Refusing them is what turns a window resize into a renderer that stops drawing, and the mapping is a checked fact rather than a hypothesis: the vocabulary area pins both funnels against each other headlessly, so the two rows are the only difference between them with no driver present. What this machine answers today is `VK_SUCCESS` for all four acquires of the GPU test's frame loop (probed 2026-10-26 with a temporary `fprintf` beside the acquire, probe removed and the tree re-checked clean), so `recreateWanted` stays 0 here and the two codes' GPU path is NOT exercised on this driver — the condition that produces them is a surface whose capabilities moved, i.e. a resized or rescaled window, which is the step below.
- **A recreate is a rung above the frame loop, and it is asked for by the framebuffer extent, never by the window's requested extent.** `xud_renderer_resize()` is the reader of `XudFrame.recreateWanted` and it runs **between draws**: the swapchain is the context's, so a recreate replaces the images, the framebuffer rows and the fence indices at once, which is a state no frame loop mid-draw can be holding. The order is the one that is correct — frame closed, framebuffer array closed, swapchain recreated, depth attachment recreated, rows reopened, frame reopened — and the depth attachment is recreated because `VUID-VkFramebufferCreateInfo-attachments-02702` makes every attachment the framebuffer's own extent, so a new swapchain extent forces a new depth buffer. The batch, the pipeline and the descriptor are **not** touched: none of them names an extent, so a resize that rebuilt them would be more work than the resize needs. The extent asked for is `xud_window_framebuffer_size()`'s (GLFW's `glfwGetFramebufferSize`, whose Wayland half keeps `wl.fbWidth`/`wl.fbHeight` scaled out of `wl.width`/`wl.height` by the fractional or buffer scale) and never `window->width`/`window->height`: on a scaled session the pair differs, and a swapchain at the requested one is a swapchain the compositor calls out of date on every acquire, which is a resize loop rather than a resize. **A recreate owes the same wait-idle guard a destroy does**: `vkCreateSwapchainKHR`'s `oldSwapchain` argument retires the old handle, and `VUID-vkDestroySwapchainKHR-swapchain-00965` ("all uses of presentable images acquired from swapchain must have completed execution") applies to that retirement — probed 2026-10-26 on this machine: a resize taken after a presented frame answered exactly that, "can't be called on a `VkSwapchainKHR` that is currently in use by a `VkQueue`", so `xud_swapchain_recreate()` runs `xud_device_wait_idle()` after its queries and before the create, and a recreate whose guard cannot run has torn nothing down. The depth recreate's own guard is the same one, in `xud_renderpass_recreate_depth()`, and its reason is the same in the other direction: the frame loop may still have a submission reading the depth buffer the recreate is about to replace. A recreate at the extent already in hand is `XUD_ALREADY`, not a second recreate, and a resize whose framebuffer extent already matches the swapchain clears the flag rather than looping.
- **Subpass discipline inside one render pass**: declare dependencies, don't rely on implicit ordering; blending is done with explicit pipeline variants (opaque = blend disabled, transparent = blend enabled + **caller-ordered**), which is raylib's painter's-algorithm semantics read literally — the library never sorts, the caller draws back-to-front, and no sort key is added to the batch's draw row (settled 2026-10-26, see *todo.md*'s "The transparent variant's three open questions"). The variants are one **array indexed by the batch's `variant` field** (`pipeline[XUD_BATCH_VARIANT_COUNT]`) under **one layout** — the two differ only in `VkPipelineColorBlendAttachmentState`, which is pipeline state, not interface state — and the transparent variant's attachment is `blendEnable = VK_TRUE` with `VK_BLEND_FACTOR_SRC_ALPHA`/`VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA` + `VK_BLEND_OP_ADD`, the Vulkan form of raylib's default `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)` + `GL_FUNC_ADD`; depth writes stay **on** under it, which is what raylib does — turning them off is a later knob, not this rule.
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
BeginDrawing: acquire image (semaphore) → reset+begin cmd → viewport/scissor
  draw calls: append each attribute into its own dynamic stream buffer, indices into the dynamic IB (SoA,
              settled 2026-10-23); the frame loop is the flush — the pending run is closed into a draw row
              before the command buffer is begun, and xud_batch_reset() runs at the end of a frame that
              completed (xud_batch_should_flush() is the mid-frame decision and no caller acts on it yet)
  before the pass: HOST_WRITE→TRANSFER_READ barrier (ONE call covering both arenas' host writes — a barrier
              is a scope, not a buffer) → five vkCmdCopyBuffer (xud_batch_upload, live bytes only) → TRANSFER_WRITE→
              VERTEX_ATTRIBUTE_READ barrier → one vkCmdBindVertexBuffers over the four streams → one vkCmdBindIndexBuffer
              (vkCmdDrawIndexed names no buffer on this header) → the block's ONE vkCmdCopyBuffer (xud_descriptor_upload,
              every frame, whether or not the batch has content, because the shader reads the block) → TRANSFER_WRITE→
              UNIFORM_READ barrier (a SECOND call: the spec's supported-access-type rule binds UNIFORM_READ to the two
              shader stages and VERTEX_ATTRIBUTE_READ to the input+vertex stages, so one barrier naming both access bits
              with one stage mask naming all three is refused) → one vkCmdBindDescriptorSets (set 0, against the
              pipeline's OWN layout, firstSet 0 count 1)
  begin render pass → one vkCmdBindPipeline + vkCmdPushConstants per recorded draw row (the bind names
              the variant's row of the pipeline list — the last bind before a draw is the one that draw uses,
              which is the Vulkan form of rlgl re-selecting state per batch; the push is against the layout's
              OWN recorded range, stageFlags/offset/size read off descriptor->pushRange, not constants of the
              loop's) → one vkCmdDrawIndexed per recorded draw row → end render pass
EndDrawing: end cmd → submit (graphics, acquire semaphore waited, per-image fence) → present (queue)
            → wait+reset that fence → reset the batch
```

## Testing rules

- Every public function gets at least one test in `tests/`.
- Split pure logic from GPU code so the default test run passes **with no device present**.
- GPU tests are gated by `XUD_GPU_TESTS=1`; the default run must stay green on a machine with no driver.
- Tests must be deterministic — seed any RNG from a fixed test seed.
- A failing test blocks the commit. Run the test binary (and the `-fsanitizer` build) before committing — see the *Commit rule* below for the full bar.
- Validation-layer output is a test signal: a GPU test run in debug must be validation-clean, not just "didn't crash".
- **`tests/` is one TU per area** (split 2026-10-25, see *Verified test split*): `xud_test_harness.h`/`.c` own the
    two counters, the one named check and the GPU gate — an area never redeclares them — and `xud_test_main.c` is
    the runner, one call per area, in layer-ladder order. An area includes `xud_test_harness.h` plus only the layer
    headers it actually names, gates its own GPU half inside its own entry point, and never calls another area. New
    tests go in the area file that matches the rung; a new rung gets a new area file and one line in the runner and
    in `CMakeLists.txt`'s explicit source list.

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

## Open design decisions (ask, don't assume)

- **Model format**: glTF 2.0 via vendored cgltf (raylib does exactly this) vs a minimal `.glb` reader of our own.
- **Audio backend**: vendor miniaudio (raylib's `raudio` does) vs no audio yet.
- **Text/font**: bitmap-font atlas of our own vs vendored `stb_truetype`/fontstash.
