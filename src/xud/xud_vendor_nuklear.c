/***********************************************************************************************
 *
 *   xud_vendor_nuklear.c - the one TU that compiles nuklear.h into the library
 *
 *   Owns : the compiled body of nuklear.h - the entire single-header gui: the layout, the widget
 *          set, the command queue and the nk_convert it is converted by.
 *   Layer: the vendored third-party TU, the one place in the tree that defines NK_IMPLEMENTATION.
 *
 *   Every other file sees nuklear declaration-only through xud_nuklear.h; this file is the single
 *   compiled copy, the same single-vendor-TU role as the glfw, stb and miniaudio vendoring. The
 *   feature set itself is named once in xud_nuklear.h so this TU and its declaration-only
 *   consumers can never drift apart.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#define NK_IMPLEMENTATION

#include <stdlib.h> /* Required for: malloc / free, the two STBTT hooks below */

/* The font baker calls the stb_truetype API, which is implemented once in xud_vendor_stb_truetype
   .c. Nuklear's fallback for an undefined hook is a static file-scope copy, and those two copies
   would then be unused here and read as an error under the -Werror the build carries. Naming the
   hooks before nuklear.h makes its fallbacks compile out and the calls bind to the vendored
   implementation. */
#define STBTT_malloc(size, data) malloc(size)
#define STBTT_free(ptr, data) free(ptr)

#include "xud_nuklear.h" /* Required for: the nuklear feature set and, with NK_IMPLEMENTATION defined above, the header's one compiled copy */
