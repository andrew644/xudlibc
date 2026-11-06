/***********************************************************************************************
 *
 *   xud_ui.h - the ui rung: one nuklear immediate-mode context, its one baked font atlas and the
 *              fold of a nuklear frame's converted vertices into the batch
 *
 *   Owns : XudUi - a nuklear nk_context, the nk_font_atlas that bakes the caller's font file into
 *          one RGBA32 image, that image as the context's one TEXTURE (and the set-1 sampler slot
 *          it occupies), the null texture nk_font_atlas_end() fills with the atlas's own white
 *          pixel, and the two host staging arrays nk_convert() writes a frame's vertices and
 *          elements into. The pure halves - the one vertex mapper, the draw-list -> batch fold and
 *          the input feed - take their data as arguments, which is what makes them answerable with
 *          no context and no driver present.
 *   Layer: above the batch, the texture, the descriptor and the input rungs, and the one place in
 *          the tree that sees nuklear.h (the vendor TU compiles its NK_IMPLEMENTATION and every
 *          other TU here includes the header declaration-only through this file).
 *
 *   Why the fold is into the shared batch rather than a buffer of the ui's own: the batch's arena
 *          is where every per-frame geometry lives, and the ui is per-frame geometry. One texture
 *          carries the whole frame - nk_font_atlas_end() names the atlas's set-1 slot as the
 *          font's texture AND points config.tex_null at the atlas's white pixel, so glyph and
 *          plain-fill commands both draw through the textured column with that one slot, which is
 *          what lets the fold be one draw row, the same shape the font walk records.
 *
 *   What this rung does NOT own: the window, the frame loop, the camera (the ui is screen-space
 *          and assumes the identity viewProj, so a live camera warps it - a noted limitation), and
 *          the per-command clip rects (windows overlap by paint order, not by a scissor).
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#ifndef XUD_XUD_UI_H_
#define XUD_XUD_UI_H_

#include "xud.h" /* Required for: XudResult, XudContext, XudHandle, XudFontData - the public surface this rung is a rung of */
#include "xud_batch.h" /* Required for: XudBatch and the appends the fold writes through */
#include "xud_input.h" /* Required for: XudInput, the snapshot the input feed reads */
#include "xud_nuklear.h" /* Required for: struct nk_context / nk_font_atlas / nk_draw_null_texture, the nk_input_* feed, nk_convert and the vertex-layout tokens - declaration only, the NK_IMPLEMENTATION lives in xud_vendor_nuklear.c */
#include <stdint.h> /* Required for: uint8_t / uint16_t / uint32_t, the vertex staging and the element staging */

/*-----------------------------------------------------------------------------------------------
 * The staging, and the one vertex layout nk_convert() writes
 *-----------------------------------------------------------------------------------------------*/

/* The host arrays nk_convert() fills a frame's geometry into. Sized for the batch's own bound
   rather than one frame's worth of widgets: nk_convert writes into these FIRST and the fold
   checks the result against the batch's live capacity AFTER, so a frame whose ui is bigger than
   the batch still converts in full and is then refused as one unit rather than truncated in the
   middle of a command. The vertex bound is a power of two below nk's 16-bit index ceiling, which
   is what keeps every element nk writes legal for a staging that big. */
#define XUD_UI_VERTEX_STAGING 16384u
#define XUD_UI_ELEMENT_STAGING 65536u

/* One vertex of the ui, the 20-byte layout the reference glfw backend uses and the one the fold
   maps into the batch's four SoA streams. pos is the framebuffer's own pixel space (y down), uv
   is the atlas's normalised [0,1] space and col is the batch's R8G8B8A8 already. */
typedef struct XudUiVertex {
    float pos[2];
    float uv[2];
    uint8_t col[4];
} XudUiVertex;

/*-----------------------------------------------------------------------------------------------
 * The rung
 *-----------------------------------------------------------------------------------------------*/

typedef struct XudUi XudUi;
struct XudUi {
    XudContext* context; /* BORROWED: the context the rung was opened above, whose input, batch and extent the frame halves read */
    struct nk_context nk; /* the immediate-mode context; its own memory is the standard library's until nk_free() */
    struct nk_font_atlas atlas; /* the baked atlas: the glyph rows and the pixel image they came from */
    struct nk_draw_null_texture texNull; /* the atlas's white pixel, what shape drawing samples */
    struct nk_font* font; /* the one font the context draws with, the atlas's first */
    XudHandle atlasTexture; /* the TEXTURE handle the atlas's image was opened as */
    uint32_t atlasSlot; /* the entry of set 1's sampler array the record's view and sampler occupy */
    uint32_t fbWidth; /* the framebuffer extent the last map used, pixels */
    uint32_t fbHeight; /* ditto, the vertical half */
    XudUiVertex* vertexStaging; /* XUD_UI_VERTEX_STAGING vertices, nk_convert's vertex target */
    uint16_t* elementStaging; /* XUD_UI_ELEMENT_STAGING indices, nk_convert's element target */
    struct nk_buffer commands; /* the draw-command buffer nk_convert appends a frame's commands into */
};

/*-----------------------------------------------------------------------------------------------
 * The pure halves, headless-testable
 *-----------------------------------------------------------------------------------------------*/

/// Pure: map one converted vertex into the batch's four attribute values. pos comes out of the
/// framebuffer's pixel space (y down, 0..fbWidth / 0..fbHeight) as this rung's y-up clip space
/// under the identity matrices - pos.x = 2x/fbWidth - 1, pos.y = 1 - 2y/fbHeight, which is the
/// inverse of the vertex stage's own final Y-flip - and z is 0, the screen plane. uv is copied
/// through and col is the R8G8B8A8 bytes as-is. XUD_INVALID_ARG on a NULL vertex or out-row, or on
/// a zero extent (a divide by a missing framebuffer is an inf in a float stream).
XudResult xud_ui_map_vertex(
    const XudUiVertex* vertex, uint32_t fbWidth, uint32_t fbHeight, float outPos[3], float outUv[2], uint8_t outColor[4]);

/// Pure: fold one converted frame into the batch - save the batch's variant/material/texture state,
/// move it to the textured row of the atlas's slot, append the mapped vertices at the live vertex
/// count and the elements with that count as their base, and move the state back, which closes the
/// run into one draw row. A frame whose vertices or indices do not fit the batch's live capacity
/// notes the capacity ask and appends nothing, which defers the whole ui to the frame after the
/// boundary's grow. XUD_INVALID_ARG on a NULL batch, vertices or elements; XUD_OK otherwise.
XudResult xud_ui_append_draw_list(XudBatch* batch, const XudUiVertex* vertices, uint32_t vertexCount, const uint16_t* elements,
    uint32_t elementCount, uint32_t fbWidth, uint32_t fbHeight, uint32_t atlasSlot);

/// Pure: feed one frame of input into a nuklear context from the input rung's snapshot - the text
/// runes, the key states (the modifiers and the arrow/context-sensitive keys read the way the
/// reference backend reads them), the cursor position, the three mouse buttons and the wheel
/// delta. This is the half the headless test drives against a hand-written snapshot and a context
/// opened with nk_init_fixed. XUD_INVALID_ARG on a NULL context or snapshot.
XudResult xud_ui_feed_input(struct nk_context* context, const XudInput* input);

/*-----------------------------------------------------------------------------------------------
 * The driver halves
 *-----------------------------------------------------------------------------------------------*/

/// Driver: bring the rung up above a live context - bake the caller's font file (or the built-in
/// default when font is NULL) into the atlas, open that image as the context's one TEXTURE and
/// bind it into set 1, name the atlas's texture and white pixel, and open the immediate-mode
/// context on the atlas's font. XUD_INVALID_ARG on a NULL out-param or context; XUD_UNSUPPORTED on
/// a context that has not completed bring-up (no handle table, texture, descriptor or device).
XudResult xud_ui_open(XudUi** outUi, XudContext* context, const XudFontData* font);

/// Driver: the reverse of open - close the atlas's TEXTURE through the context's own close (which
/// gives its set-1 slot back and retires the image, the view and the sampler), free the nuklear
/// context, clear the atlas and free the rung. XUD_INVALID_ARG on a NULL out-param; XUD_ALREADY on
/// a NULL rung, which is what a context that never opened the ui answers.
XudResult xud_ui_destroy(XudUi** ioUi, XudContext* context);

/// Driver: open the frame's input - feed the context from the context's own input rung. XUD_
/// INVALID_ARG on a NULL rung; XUD_UNSUPPORTED on a rung with no input rung to feed from.
XudResult xud_ui_frame_begin(XudUi* ui);

/// Driver: close the frame and fold it into the batch - nk_end(), nk_convert() into the staging,
/// and the pure fold above against the context's batch at the context's framebuffer extent. The
/// caller's widgets were appended to the context between begin and this call. XUD_INVALID_ARG on a
/// NULL rung; XUD_UNSUPPORTED on a rung whose context has no batch to fold into.
XudResult xud_ui_frame_draw(XudUi* ui);

#endif /* XUD_XUD_UI_H_ */
