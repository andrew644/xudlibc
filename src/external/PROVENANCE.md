# Vendored third-party sources — provenance

**These are upstream sources the library builds (or will build) against — not reference material.**
Unlike `reference/raylib/`, this bucket **is committed** (decided 2026-10-23), **with its license files**, which
must stay with the code. Never edit these files in place; when a version bump is wanted, re-vendor from upstream.

Copied into `src/external/` on two dates: **2026-10-23** (Volk + the GLFW tree) and **2026-11-01** (the six
single-header libs — commit `Add extra libraries`). The exact upstream commit/tag was **not recorded at either
copy time**; everything below is what the files themselves declare, re-read here.

## What is in this bucket

| Path | Size | What it is |
| --- | --- | --- |
| `volk.h` | 190 KB (3493 lines) | **Volk** — the Vulkan loader + entry-point table. `VOLK_HEADER_VERSION 366`; head says "Report bugs and download new versions at https://github.com/zeux/volk", Copyright (C) 2018-2026 Arseny Kapoulkine. Declares **842** `extern PFN_vkXxx vkXxx;` entries. The MIT notice is at the **end of this file** — it must stay with it. |
| `volk.c` | 340 KB (4284 lines) | Volk's implementation half (`volkInitialize` → `dlopen("libvulkan.so.1", RTLD_NOW\|RTLD_LOCAL)` at line 114, fallback `libvulkan.so` 116). |
| `glfw/` | 2.2 MB, 86 files | **GLFW 3.5.1**, full upstream tree: `include/GLFW/glfw3.h` (`GLFW_VERSION_MAJOR 3` / `MINOR 5` / `REVISION 1`) + `glfw3native.h`; `src/` 61 files (35 `.c`, 18 `.h`, 5 `.m`, incl. the `wl_*.c`/`x11_*.c`/`cocoa_*`/`macos_*`/`win32_*` platform halves and the `wgl_`/`glx_`/`egl_`/`osmesa_`/`nsgl_` context halves, `vulkan.c`, `mappings.h`+`mappings.h.in`); `deps/wayland/` 9 protocol XMLs; `CMakeLists.txt` + `src/CMakeLists.txt` + `CMake/`; `LICENSE.md`, `CONTRIBUTORS.md`, `README.md`. |
| `cgltf.h` | 201 KB (7240 lines) | **cgltf 1.15** — a single-file glTF 2.0 parser (C99). Guard `CGLTF_H_INCLUDED__` (line 94), implementation macro `CGLTF_IMPLEMENTATION`. Head: "https://github.com/jkuhlmann/cgltf". MIT notice at the **end of the file**, Copyright (c) 2018-2021 Johannes Kuhlmann (line 7221). |
| `miniaudio.h` | 3.9 MB (95864 lines) | **miniaudio v0.11.25, dated 2026-03-04** — audio playback and capture. Guard `miniaudio_h` (line 3738), implementation macro `MINIAUDIO_IMPLEMENTATION` (or the `MA_IMPLEMENTATION` alias). Head: https://miniaud.io, David Reid. "Choice of public domain or MIT-0", both notices at the **end of the file** (unlicense + MIT No Attribution, Copyright 2026 David Reid). Built-in WAV/FLAC/MP3 decoders are ON by default (`MA_NO_WAV`/`MA_NO_FLAC`/`MA_NO_MP3` are opt-outs); the Linux device backends (ALSA/PulseAudio/JACK/sndio/OSS/AAudio) are `dlopen`ed. |
| `stb_image.h` | 276 KB (7988 lines) | **stb_image v2.30** — image loader (PNG/JPG/HDR/…). Guard `STBI_INCLUDE_STB_IMAGE_H`, implementation macro `STB_IMAGE_IMPLEMENTATION`. Public domain, "no warranty implied; use at your own risk", http://nothings.org/stb. |
| `stb_image_resize2.h` | 447 KB (10679 lines) | **stb_image_resize2 v2.18** — image resizing (SSE2/AVX/NEON/WASM). Guard `STBIR_INCLUDE_STB_IMAGE_RESIZE2_H`, implementation macros `STB_IMAGE_RESIZE_IMPLEMENTATION` / `STB_IMAGE_RESIZE2_IMPLEMENTATION`. Public domain, http://github.com/nothings/stb, v2 by Jeff Roberts and Jorge L Rodriguez. |
| `stb_image_write.h` | 70 KB (1724 lines) | **stb_image_write v1.16** — writes PNG/BMP/TGA/JPEG/HDR to stdio. Guard `INCLUDE_STB_IMAGE_WRITE_H`, implementation macro `STB_IMAGE_WRITE_IMPLEMENTATION`. Public domain, Sean Barrett 2010-2015. |
| `stb_truetype.h` | 195 KB (5079 lines) | **stb_truetype v1.26** — TrueType rasterizer/bake, authored 2009-2021 by Sean Barrett / RAD Game Tools. Guard `__STB_INCLUDE_STB_TRUETYPE_H__`, implementation macro `STB_TRUETYPE_IMPLEMENTATION`. Carries an upstream banner: "NO SECURITY GUARANTEE — DO NOT USE THIS ON UNTRUSTED FONT FILES" (it does no range checking of the offsets in a font file). Public domain notice at the **end of the file**. |

## Licenses (all stay in the tree, next to the code)

- **Volk** — MIT, notice at the end of `volk.h` (Copyright © 2018-2026 Arseny Kapoulkine).
- **GLFW** — zlib/libpng license, `glfw/LICENSE.md` (Copyright © 2002-2006 Marcus Geelnard, 2006-2019 Camilla
  Löwy); `glfw/README.md` points at https://www.glfw.org/license.html.
- **cgltf** — MIT, notice at the end of `cgltf.h`.
- **miniaudio** — public domain **or** MIT-0, both notices at the end of `miniaudio.h`.
- **the four stb headers** — public domain ("this is free software… we make this dedication… to the public at
  large"), notice at the top of each file and the full text at the end of each.

No separate `LICENSE` file was needed for the single-header libs: **each notice travels inside its own file**,
which is what "committed with its license files" means for this bucket.

## What is built, and what is not

**Built into `build/libxud.a` today**: `volk.c.o` + the 21 GLFW objects (`context.c.o` … `wl_window.c.o`) + the
24 xud module objects = **46** objects (verified 2026-11-01 with `ar -t`). The generated
`-client-protocol-code.h` files are not separate objects — CMake does not compile a `.h` source, and
`glfw/src/wl_init.c` 59–91 `#include`s them into its own body, so the protocol stubs are local symbols inside
`wl_init.c.o`. Neither `nm -u build/xud_test` nor `nm -u build/hello_context` lists a `vk*` or `glfw*` symbol,
and `ldd` lists no `libvulkan`, no `libglfw` and no `libGL`/`libEGL`.

**Not built: the six single-header libs.** Nothing in `src/xud/`, `tests/` or `examples/` includes `cgltf.h`,
`miniaudio.h` or any `stb_*.h` yet — they are **committed, not consumed**, so they add no object to the archive
and no line to any binary. That is the state of the vendoring decision, not a half-built subsystem: the model
loader, the audio wrapper and the text/font rung are still open items in `todo.md`.

Verified 2026-11-01 (a scratch probe in `/tmp`, deleted after, never in the tree) under the repo's own flags:

- **Declarations only** — all six are clean under `-std=c11 -Wall -Wextra -Wpedantic -Werror` with **both** gcc
  16.2.1 and clang 22.1.8.
- **Implementation halves** — all clean **except `stb_image_resize2.h`**, which gcc 16.2.1 refuses under
  `-Wpedantic` (`-Werror=pedantic`: "initializer element is not a constant expression" at `stb_image_resize2.h`
  1468, the `STBIR__CONST_4_32i` macro's cast inside a `static` initializer). The same TU is clean under gcc
  **without** `-Wpedantic` and clean under clang 22.1.8 **with** it. So an implementation TU for that header
  needs the same flag relaxation the GLFW TUs already got (`-Wall -Werror`, no `-Wextra`/`-Wpedantic`) — a
  separate target, not the repo's default flags.

## Git policy

This bucket is committed with its license files; `reference/` (including `reference/raylib/`) is never committed
and never edited.
