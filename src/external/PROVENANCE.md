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

- **Loader**: Volk replaces the hand-rolled `xud_loader.c` PFN table — the rework is still open work in `todo.md`.
- **Window/input**: GLFW, **Wayland** backend (`GLFW_BUILD_WAYLAND` → `_GLFW_WAYLAND` in `glfw/src/CMakeLists.txt`
  54–55, protocol code generated from `deps/wayland/*.xml` by `wayland-scanner`, 75–107), not the system package.
- **Git policy**: this bucket is committed with its license files; `reference/` is never committed.

**Not yet true** (fix when the build lands): the repo-root `CMakeLists.txt` references neither `src/external/volk.*`
nor `src/external/glfw`, and nothing in `src/xud/` includes `volk.h` — the landed `xud_loader.c` table is still what
runs. Nothing here is built into `build/libxud.a` today.
