/***********************************************************************************************
 *
 *   xud_nuklear.h - the one place the nuklear feature set is named, and the one include of its
 *                   header the whole tree goes through
 *
 *   Why a header at all: a single-header library's feature macros decide which declarations it
 *          exposes as well as which code it compiles, so the NK_IMPLEMENTATION TU (the vendor)
 *          and every declaration-only consumer must define the SAME set or the consumers see a
 *          different nuklear.h than the one that was built. Naming the set once here and having
 *          both sides include this file is what keeps them the same set rather than two copies.
 *          Only the vendor TU additionally defines NK_IMPLEMENTATION (and its two STBTT hooks),
 *          which is what turns the header from declarations into the one compiled copy.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#ifndef XUD_XUD_NUKLEAR_H_
#define XUD_XUD_NUKLEAR_H_

/* The standard allocator (nk_init_default / nk_buffer_init_default) and the fixed integer and
   boolean types, so the tree's own int widths and _Bool are the ones nuklear is stamped with. */
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_BOOL
#define NK_INCLUDE_STANDARD_VARARGS
/* The vertex/element output (nk_convert and the vertex layout) and the font baking (the atlas and
   the nk_font_atlas_* row) are the two halves the ui rung lives in. The default font is what the
   atlas falls back to when the caller hands no font data of its own. */
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
/* stb_truetype is implemented once in xud_vendor_stb_truetype.c, not inside nuklear.h. */
#define NK_NO_STB_TRUETYPE_IMPLEMENTATION
/* The ui feeds key state every frame rather than key events, so a key reads as clicked only on a
   state change rather than on every frame it is held. */
#define NK_KEYSTATE_BASED_INPUT

#include "nuklear.h" /* Required for: the whole nuklear API, declaration-only except in the vendor TU that defines NK_IMPLEMENTATION above this include */

#endif /* XUD_XUD_NUKLEAR_H_ */
