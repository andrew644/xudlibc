/***********************************************************************************************
 *
 *   xud_test_ui.c - the ui rung's headless half: the vertex mapper, the draw-list -> batch fold,
 *          the input feed and the atlas bake, none of it handed to a driver
 *
 *   Owns : the pixel-to-clip arithmetic of xud_ui_map_vertex() and its refusals, the fold of a
 *          converted frame into the shared batch (the state save and restore, the mapped vertices
 *          and the element base offset, and the whole-frame capacity refusal), the input feed read
 *          back off a nuklear context opened with nk_init_fixed, and the one default-font atlas
 *          bake that proves the vendored stb_truetype and the feature set in xud_nuklear.h bake.
 *   Layer: tests. The pure halves take their data as arguments, so the whole arithmetic runs with
 *          no context and no device present, the same way the batch's and the font's pure halves
 *          do.
 *
 *   What this area does NOT check, and why: the atlas TEXTURE's open and close, the nk_convert of
 *          a live context's widgets into the staging, and the fold through a real batch's driver
 *          upload. The first two need a device (a texture is a driver object and a convert reads a
 *          context's own command queue), and the third needs the frame loop's command buffer.
 *
 *   Deterministic: fixed vertex rows, a hand-written input snapshot, the built-in default font.
 *          No time, no randomness, no filesystem.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud.h" /* Required for: XudResult, the XUD_KEY_* tokens the input feed reads, XUD_GPU_TESTS */
#include "xud_batch.h" /* Required for: XudBatch, the arena the fold writes into and the constants its offsets are stated against */
#include "xud_input.h" /* Required for: XudInput and the XUD_INPUT_* tokens the snapshot the feed reads is spelled in */
#include "xud_test_harness.h" /* Required for: xud_check, the area entry point */
#include "xud_ui.h" /* Required for: XudUiVertex, the pure halves under test and the nuklear declarations they are written against */
#include <math.h> /* Required for: fabsf, the float compares on the mapped clip-space positions */
#include <stdlib.h> /* Required for: the nk_init_fixed memory the feed test opens its context in */
#include <string.h> /* Required for: memset, the batch and snapshot rows this area zeroes */

/* A host arena the headless folds write into, the same one the batch area's appends check
   against: the fold's whole point is a write at a computed offset, and the only thing that makes
   that testable with no driver present is real bytes to compare at the offset the layout computed.
*/
static uint8_t xudTestUiArena[(XUD_BATCH_MAX_VERTICES * XUD_BATCH_VERTEX_SIZE)
    + (XUD_BATCH_INDEX_CAPACITY(XUD_BATCH_MAX_VERTICES) * XUD_BATCH_INDEX_SIZE)];

/*-----------------------------------------------------------------------------------------------
 * The vertex mapper: the pixel-to-clip arithmetic and the refusals that guard a divide by a
 * missing framebuffer.
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_ui_map_vertex(void) {
    float pos[3];
    float uv[2];
    uint8_t color[4];
    XudUiVertex origin = { { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 10u, 20u, 30u, 255u } };
    XudUiVertex corner = { { 800.0f, 600.0f }, { 1.0f, 1.0f }, { 0u, 0u, 0u, 0u } };
    XudUiVertex centre = { { 400.0f, 300.0f }, { 0.5f, 0.25f }, { 1u, 2u, 3u, 4u } };

    xud_check(xud_ui_map_vertex(NULL, 800u, 600u, pos, uv, color) == XUD_INVALID_ARG, "ui/map-null-vertex",
        "a vertex that cannot be read is refused before anything is divided");
    xud_check(xud_ui_map_vertex(&origin, 800u, 600u, NULL, uv, color) == XUD_INVALID_ARG, "ui/map-null-pos",
        "and a position row with nowhere to be written is refused");
    xud_check(
        xud_ui_map_vertex(&origin, 800u, 600u, pos, NULL, color) == XUD_INVALID_ARG, "ui/map-null-uv", "and the uv row's own refusal");
    xud_check(xud_ui_map_vertex(&origin, 800u, 600u, pos, uv, NULL) == XUD_INVALID_ARG, "ui/map-null-color", "and the colour row's");
    xud_check(xud_ui_map_vertex(&origin, 0u, 600u, pos, uv, color) == XUD_INVALID_ARG, "ui/map-zero-width",
        "a framebuffer with no width is a divide by zero, refused rather than an inf in a float stream");
    xud_check(xud_ui_map_vertex(&origin, 800u, 0u, pos, uv, color) == XUD_INVALID_ARG, "ui/map-zero-height", "and the same for no height");

    xud_check(xud_ui_map_vertex(&origin, 800u, 600u, pos, uv, color) == XUD_OK, "ui/map-origin-ok", "the top-left pixel maps");
    xud_check((pos[0] == -1.0f) && (pos[1] == 1.0f) && (pos[2] == 0.0f), "ui/map-origin-clip",
        "the top-left pixel is the top-left of the clip space, y-up: the y-down pixel space flipped by the inverse of the vertex stage's "
        "own flip");
    xud_check((uv[0] == 0.0f) && (uv[1] == 0.0f), "ui/map-origin-uv", "the uv is copied through, not remapped");
    xud_check((color[0] == 10u) && (color[1] == 20u) && (color[2] == 30u) && (color[3] == 255u), "ui/map-origin-color",
        "and the R8G8B8A8 bytes are the batch's already, copied byte for byte");

    xud_check(xud_ui_map_vertex(&corner, 800u, 600u, pos, uv, color) == XUD_OK, "ui/map-corner-ok", "the bottom-right pixel maps");
    xud_check((fabsf(pos[0] - 1.0f) < 1e-6f) && (fabsf(pos[1] + 1.0f) < 1e-6f) && (pos[2] == 0.0f), "ui/map-corner-clip",
        "the bottom-right pixel is the bottom-right of the clip space: the y flip is the whole of the transform's vertical half");
    xud_check((uv[0] == 1.0f) && (uv[1] == 1.0f), "ui/map-corner-uv", "and the uv at the far corner of the atlas is copied too");

    xud_check(xud_ui_map_vertex(&centre, 800u, 600u, pos, uv, color) == XUD_OK, "ui/map-centre-ok", "the centre pixel maps");
    xud_check((fabsf(pos[0]) < 1e-6f) && (fabsf(pos[1]) < 1e-6f) && (pos[2] == 0.0f), "ui/map-centre-clip",
        "the centre of the framebuffer is the centre of the clip space, which is what the affine half of the map has to land on");
    xud_check((fabsf(uv[0] - 0.5f) < 1e-6f) && (fabsf(uv[1] - 0.25f) < 1e-6f), "ui/map-centre-uv",
        "and a mid-atlas uv is copied at its own value, the mapper's only uv duty");
}

/*-----------------------------------------------------------------------------------------------
 * The fold: a converted frame into the shared batch, the state save and restore, the element base
 * offset, and the whole-frame capacity refusal.
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_ui_append(void) {
    XudBatch b;
    XudBatch empty;
    XudBatch full;
    uint32_t asked = 0u;
    const float* originPos = NULL;
    const uint32_t* indices = NULL;
    XudUiVertex quad[4];
    uint16_t triangle[6];

    /* The quad's four vertices in the framebuffer's pixel space, and the two triangles that name
       them: a frame small enough to fit the batch's initial capacity, which is what makes the fold
       below a success rather than a refusal. */
    quad[0].pos[0] = 0.0f;
    quad[0].pos[1] = 0.0f;
    quad[1].pos[0] = 100.0f;
    quad[1].pos[1] = 0.0f;
    quad[2].pos[0] = 100.0f;
    quad[2].pos[1] = 50.0f;
    quad[3].pos[0] = 0.0f;
    quad[3].pos[1] = 50.0f;
    for (int i = 0; i < 4; i++) {
        quad[i].uv[0] = 0.0f;
        quad[i].uv[1] = 0.0f;
        quad[i].col[0] = 255u;
        quad[i].col[1] = 0u;
        quad[i].col[2] = 0u;
        quad[i].col[3] = 255u;
    }
    triangle[0] = 0u;
    triangle[1] = 1u;
    triangle[2] = 2u;
    triangle[3] = 0u;
    triangle[4] = 2u;
    triangle[5] = 3u;

    memset(&b, 0, sizeof(b));
    xud_check(xud_batch_layout(&b, XUD_BATCH_INITIAL_VERTICES) == XUD_OK, "ui/fold-layout", "the batch lays out");
    b.stagingMapped = xudTestUiArena;
    b.stagingMappedLive = 1u;

    xud_check(xud_ui_append_draw_list(NULL, quad, 4u, triangle, 6u, 800u, 600u, 2u) == XUD_INVALID_ARG, "ui/fold-null-batch",
        "a batch with no state to fold into is a bad argument");
    xud_check(xud_ui_append_draw_list(&b, NULL, 4u, triangle, 6u, 800u, 600u, 2u) == XUD_INVALID_ARG, "ui/fold-null-vertices",
        "and four vertices that cannot be read");
    xud_check(xud_ui_append_draw_list(&b, quad, 4u, NULL, 6u, 800u, 600u, 2u) == XUD_INVALID_ARG, "ui/fold-null-elements",
        "and six elements that cannot be read");
    xud_check(xud_ui_append_draw_list(&b, quad, 4u, triangle, 6u, 0u, 600u, 2u) == XUD_INVALID_ARG, "ui/fold-zero-width",
        "a framebuffer with no width is the mapper's divide-by-zero, refused here before a vertex is mapped");
    xud_check(xud_ui_append_draw_list(&b, quad, 4u, triangle, 6u, 800u, 0u, 2u) == XUD_INVALID_ARG, "ui/fold-zero-height",
        "and the same for no height");

    /* The state the fold must restore is moved to a triple it does not use, so a fold that forgets
       to put the state back is caught by the check after rather than passing silently. */
    xud_check(xud_batch_set_material(&b, 5u, XUD_BATCH_VARIANT_TRANSPARENT, 3u) == XUD_OK, "ui/fold-set-state",
        "the caller's state is set to a triple the fold's own does not match");

    xud_check(xud_ui_append_draw_list(&b, quad, 4u, triangle, 6u, 800u, 600u, 2u) == XUD_OK, "ui/fold-ok", "the four-vertex frame folds");
    xud_check((b.vertexCount == 4u) && (b.indexCount == 6u), "ui/fold-counts",
        "four live vertices and six live indices, the frame's own counts rather than the staging's");
    xud_check((b.currentMaterial == 5u) && (b.currentVariant == XUD_BATCH_VARIANT_TRANSPARENT) && (b.currentTexture == 3u),
        "ui/fold-state-restored",
        "the caller's triple is back in the state, which is what makes the fold one draw row of the atlas's slot rather than a state the "
        "caller has to undo");

    /* The first vertex's mapped position, read at the offset the layout computed rather than one
       this file remembers: a write at the wrong stream or the wrong stride fails it. */
    originPos = (const float*)(xudTestUiArena + b.streamOffset[XUD_BATCH_STREAM_POS]);
    xud_check((originPos[0] == -1.0f) && (originPos[1] == 1.0f) && (originPos[2] == 0.0f), "ui/fold-first-vertex-mapped",
        "the quad's top-left pixel is the clip space's top-left, written into the pos stream at the first vertex's slot");
    indices = (const uint32_t*)(xudTestUiArena + b.indexOffset);
    xud_check(
        (indices[0] == 0u) && (indices[1] == 1u) && (indices[2] == 2u) && (indices[3] == 0u) && (indices[4] == 2u) && (indices[5] == 3u),
        "ui/fold-elements-at-base-zero",
        "the batch was empty when the frame folded, so the element base is 0 and the indices are written at their own values - the base "
        "offset is the live vertex count, which is the half that shifts when the batch is not empty");

    /* A fold into a batch that already has a live vertex: the base offset moves to that count,
       which is the shift the comment above states and the only place it is observable. */
    {
        XudBatch base;
        memset(&base, 0, sizeof(base));
        xud_check(xud_batch_layout(&base, XUD_BATCH_INITIAL_VERTICES) == XUD_OK, "ui/fold-base-layout", "a second batch lays out");
        base.stagingMapped = xudTestUiArena;
        base.stagingMappedLive = 1u;
        base.vertexCount = 10u;
        base.indexCount = 0u;
        xud_check(xud_ui_append_draw_list(&base, quad, 4u, triangle, 6u, 800u, 600u, 2u) == XUD_OK, "ui/fold-base-ok",
            "the same frame folds into a batch that already holds ten vertices");
        indices = (const uint32_t*)(xudTestUiArena + base.indexOffset);
        xud_check((indices[0] == 10u) && (indices[1] == 11u) && (indices[2] == 12u) && (indices[3] == 10u) && (indices[4] == 12u)
                && (indices[5] == 13u),
            "ui/fold-elements-at-base-ten",
            "and the elements are shifted by the ten live vertices: nk's indices are 0-based over its own staging, and the batch's are "
            "0-based over the whole arena, so the base is the offset between the two");
    }

    /* An empty frame is a success that appends nothing, which is the half a frame with no widgets
       exercises. */
    memset(&empty, 0, sizeof(empty));
    xud_check(xud_batch_layout(&empty, XUD_BATCH_INITIAL_VERTICES) == XUD_OK, "ui/fold-empty-layout", "a third batch lays out");
    xud_check(xud_ui_append_draw_list(&empty, quad, 0u, triangle, 0u, 800u, 600u, 2u) == XUD_OK, "ui/fold-empty-ok",
        "zero vertices is a success");
    xud_check((empty.vertexCount == 0u) && (empty.indexCount == 0u), "ui/fold-empty-appends-nothing",
        "and it appends nothing, so a frame with no widgets costs the batch nothing");

    /* The capacity refusal: the batch holds one vertex short of full, and a four-vertex frame does
       not fit. The answer is a deferred success with nothing appended and a growth ask noted,
       which is the whole-frame deferral rather than a truncation in the middle of a command. */
    memset(&full, 0, sizeof(full));
    xud_check(xud_batch_layout(&full, XUD_BATCH_INITIAL_VERTICES) == XUD_OK, "ui/fold-full-layout", "a fourth batch lays out");
    full.stagingMapped = xudTestUiArena;
    full.stagingMappedLive = 1u;
    full.vertexCount = full.vertexCapacity - 1u;
    full.indexCount = 0u;
    xud_check(xud_ui_append_draw_list(&full, quad, 4u, triangle, 6u, 800u, 600u, 2u) == XUD_OK, "ui/fold-full-deferred",
        "a frame that does not fit is deferred, not refused - the answer is a success that appends nothing");
    xud_check(full.vertexCount == (full.vertexCapacity - 1u), "ui/fold-full-vertex-count-unchanged",
        "and the live vertex count is where it was, so no half frame was written");
    xud_check(full.indexCount == 0u, "ui/fold-full-index-count-unchanged",
        "and the live index count is too, which is the whole frame going, not one vertex of it");
    xud_check((xud_batch_growth_asked(&full, &asked) == XUD_OK) && (asked == 1u), "ui/fold-full-growth-asked",
        "and the refusal left a growth ask for the frame boundary above, which is what grows the batch the frame after and lets the ui "
        "reappear");
}

/*-----------------------------------------------------------------------------------------------
 * The input feed, read back off a nuklear context opened with nk_init_fixed: the key states, the
 * modifiers and the mouse buttons the feed writes are the states nk's own queries read.
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_ui_feed_input(void) {
    static uint8_t nkMemory[65536u];
    struct nk_context ctx;
    XudInput input;

    memset(&input, 0, sizeof(input));
    xud_check(xud_ui_feed_input(NULL, &input) == XUD_INVALID_ARG, "ui/feed-null-context",
        "a nuklear context that cannot be fed is a bad argument");
    xud_check(xud_ui_feed_input(&ctx, NULL) == XUD_INVALID_ARG, "ui/feed-null-input", "and an input snapshot that cannot be read");

    if (nk_init_fixed(&ctx, nkMemory, sizeof(nkMemory), NULL) != nk_true) {
        xud_check(0, "ui/feed-context-open", "the test's own context would not open");
        return;
    }

    /* The snapshot: the left arrow and the left shift down, the left mouse button down, the
       cursor mid-window, the wheel a step down, and nothing else. Every key the feed maps that is
       not in the snapshot reads up, which is what makes the ctrl-not-down and right-mouse-not-down
       checks below a read of the feed rather than of the snapshot's zeros. */
    memset(&input, 0, sizeof(input));
    input.cursorX = 123.0;
    input.cursorY = 45.0;
    input.wheelY = 2.0;
    input.keyState[XUD_KEY_LEFT - XUD_INPUT_KEY_FIRST] = XUD_INPUT_STATE_DOWN;
    input.keyState[XUD_KEY_LEFT_SHIFT - XUD_INPUT_KEY_FIRST] = XUD_INPUT_STATE_DOWN;
    input.buttonState[0u] = XUD_INPUT_STATE_DOWN;

    xud_check(xud_ui_feed_input(&ctx, &input) == XUD_OK, "ui/feed-ok", "the frame of input feeds");
    xud_check(nk_input_is_key_down(&ctx.input, NK_KEY_LEFT) == nk_true, "ui/feed-left-down",
        "the left arrow reads down in the context the feed wrote");
    xud_check(nk_input_is_key_down(&ctx.input, NK_KEY_SHIFT) == nk_true, "ui/feed-shift-down",
        "and the left shift maps to nk's shift, the modifier the reference backend reads the same way");
    xud_check(nk_input_is_key_down(&ctx.input, NK_KEY_CTRL) == nk_false, "ui/feed-ctrl-not-down",
        "and ctrl, which the snapshot does not hold, reads up - the feed writes every key, so an absent key is an explicit up rather than "
        "a stale state");
    xud_check(nk_input_is_mouse_down(&ctx.input, NK_BUTTON_LEFT) == nk_true, "ui/feed-left-button-down",
        "the left mouse button reads down, the button the snapshot held");
    xud_check(nk_input_is_mouse_down(&ctx.input, NK_BUTTON_RIGHT) == nk_false, "ui/feed-right-button-not-down",
        "and the right button, which the snapshot did not hold, reads up");

    nk_free(&ctx);
}

/*-----------------------------------------------------------------------------------------------
 * The atlas bake: the one default-font bake that proves the vendored stb_truetype and the feature
 * set in xud_nuklear.h produce an image, a named font and a null texture at the atlas's white
 * pixel.
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_ui_atlas(void) {
    struct nk_font_atlas atlas;
    struct nk_draw_null_texture null;
    struct nk_font* font = NULL;
    const void* image = NULL;
    int width = 0;
    int height = 0;

    nk_font_atlas_init_default(&atlas);
    nk_font_atlas_begin(&atlas);
    font = nk_font_atlas_add_default(&atlas, 13.0f, NULL);
    xud_check(font != NULL, "ui/atlas-font-added", "the built-in default font adds to the atlas");
    image = nk_font_atlas_bake(&atlas, &width, &height, NK_FONT_ATLAS_RGBA32);
    xud_check(image != NULL, "ui/atlas-baked",
        "the atlas bakes to an image - the stb_truetype the feature set points at is the vendored copy, and a bake that returns nothing is "
        "a hook that bound to the wrong implementation");
    xud_check((width > 0) && (height > 0), "ui/atlas-dimensions",
        "and the image has both dimensions, which is the half the texture's open would divide by");

    /* The end names the slot on the font and fills the null texture with the atlas's own white
       pixel: that is what makes glyph and shape commands both draw through the one slot. */
    nk_font_atlas_end(&atlas, nk_handle_id(1), &null);
    xud_check(null.texture.id == 1, "ui/atlas-null-texture-id",
        "the null texture takes the atlas's own slot, the one every glyph command samples too");
    xud_check((null.uv.x > 0.0f) && (null.uv.y > 0.0f), "ui/atlas-null-uv-white-pixel",
        "and its uv is the white pixel's centre, inside the atlas rather than on its edge - the pixel shape drawing samples");

    nk_font_atlas_clear(&atlas);
}

void xud_test_ui(void) {
    xud_test_ui_map_vertex();
    xud_test_ui_append();
    xud_test_ui_feed_input();
    xud_test_ui_atlas();
}
