# Vendored third-party sources — provenance

**These are upstream sources the library builds (or will build) against — not reference material.**
Unlike `reference/raylib/`, this bucket **is committed** (decided 2026-10-23), **with its license files**, which
must stay with the code. Never edit these files in place; when a version bump is wanted, re-vendor from upstream.

Copied into `src/external/` on 2026-10-23 (raylib's own directory name for vendored deps). The exact upstream
commit/tag was **not recorded at copy time**; everything below is what the files themselves declare, re-read here.

| Path | Size | What it is |
| --- | --- | --- |
| `volk.h` | 190 KB (3493 lines) | **Volk** — the Vulkan loader + entry-point table. `VOLK_HEADER_VERSION 366`; head says "Report bugs and download new versions at https://github.com/zeux/volk", Copyright (C) 2018-2026 Arseny Kapoulkine. Declares **842** `extern PFN_vkXxx vkXxx;` entries. The MIT notice is at the **end of this file** — it must stay with it. |
| `volk.c` | 340 KB (4284 lines) | Volk's implementation half (`volkInitialize` → `dlopen("libvulkan.so.1", RTLD_NOW\|RTLD_LOCAL)` at line 114, fallback `libvulkan.so` 116). |
| `glfw/` | 2.2 MB, 86 files | **GLFW 3.5.1**, full upstream tree: `include/GLFW/glfw3.h` (`GLFW_VERSION_MAJOR 3` / `MINOR 5` / `REVISION 1`) + `glfw3native.h`; `src/` 61 files (35 `.c`, 18 `.h`, 5 `.m`, incl. the `wl_*.c`/`x11_*.c`/`cocoa_*`/`macos_*`/`win32_*` platform halves and the `wgl_`/`glx_`/`egl_`/`osmesa_`/`nsgl_` context halves, `vulkan.c`, `mappings.h`+`mappings.h.in`); `deps/wayland/` 9 protocol XMLs; `CMakeLists.txt` + `src/CMakeLists.txt` + `CMake/`; `LICENSE.md`, `CONTRIBUTORS.md`, `README.md`. |

Licenses (both stay in the tree, next to the code):

- **Volk** — MIT, notice at the end of `volk.h` (Copyright © 2018-2026 Arseny Kapoulkine).
- **GLFW** — zlib/libpng license, `glfw/LICENSE.md` (Copyright © 2002-2006 Marcus Geelnard, 2006-2019 Camilla
  Löwy); `glfw/README.md` points at https://www.glfw.org/license.html.

What is decided about them (see AGENTS.md *Settled decisions*, 2026-10-23):

- **Loader**: Volk **is built in** (S6.5, 2026-10-23) — `src/external/volk.c` is compiled into the `xud` archive and
  `src/xud/xud_loader.c` is now a thin per-context wrapper over Volk's `VolkInstanceTable`/`VolkDeviceTable`; the
  hand-rolled PFN table it replaced is gone.
- **Window/input**: GLFW, **Wayland** backend, **built in** (S6.6, 2026-10-23) — the repo-root `CMakeLists.txt` compiles
  upstream's Wayland source set as the OBJECT target `glfw_vendor` (`-D_GLFW_WAYLAND`, `-D_DEFAULT_SOURCE`, `HAVE_MEMFD_CREATE`
  from `check_function_exists`), generates the 9 `deps/wayland/*.xml` protocol pairs with `wayland-scanner` into
  `build/glfw-wayland/` (never next to these sources), compiles the vendored TUs with `-Wall -Werror` but **without**
  `-Wextra`/`-Wpedantic` (GLFW 3.5.1 is a hard error under both), and links the four Wayland libs PUBLIC into `xud` —
  no `-lglfw` anywhere. `x11_*.c`/`xkb_unicode.c` and the win32/cocoa/macos/nsgl/glx halves are not in the build.
  Nothing in `src/xud/` includes `GLFW/glfw3.h` yet, so there is still no window and no surface (that is S7).
- **Git policy**: this bucket is committed with its license files; `reference/` is never committed.

**Built into `build/libxud.a` today**: `volk.c.o` + the 21 GLFW objects (`context.c.o` … `wl_window.c.o`) + the 7 xud
module objects = **29** objects (verified 2026-10-23 with `ar -t`). The generated `-client-protocol-code.h` files are
not separate objects — CMake does not compile a `.h` source, and `glfw/src/wl_init.c` 59–91 `#include`s them into its
own body, so the protocol stubs are local symbols inside `wl_init.c.o`. Neither `nm -u build/xud_test` nor `nm -u
build/hello_context` lists a `vk*` or `glfw*` symbol, and `ldd` lists no `libvulkan`, no `libglfw` and no `libGL`/`libEGL`.
