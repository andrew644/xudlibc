/***********************************************************************************************
 *
 *   xud_font.c - the font rung: stb_truetype's own bake, the atlas's byte budget, the one TEXTURE
 *                slot this rung owns, and the load/close pair plus the metric queries xud.h
 *                declares above it
 *
 *   Owns : the bake (the font header read, the pack context opened over a host atlas, one pack
 *          range naming the description's codepoint array, the pack closed), the atlas's two host
 *          buffers and the doubling retry that decides which one of them is big enough, the cove
 *          rage-to-texel conversion, the row lookup and the measurement, and the record behind a
 *          FONT-kind slot - plus the teardown rung that runs above xud_texture_close_all().
 *   Layer: above the texture rung and below xud.h. This file is the only place in the tree that
 *          calls stb_truetype, and the only place that calls xud_texture_open() for something tha
 *          t is not a caller's own texture description: the atlas is this rung's bytes, and the on
 *          e thing it hands the rung below is the XudTextureData it stamps from them.
 *
 *   Why the retry loop lives in the open and not in the bake: the bake is the packer, and the pack
 *   er's own note above its declaration says its packing is "very crappy", so no arithmetic on this
 *   side of it predicts whether a set fits a given atlas. The dim is therefore the loop's decision
 *   (double and try again) and the bake's answer is one bit (fits / does not fit), which is what kee
 *   ps the bake pure enough to be driven headless with a hand-written dim and a real font file, and
 *   the retry honest about what actually costs the retry.
 *
 *   Why the atlas's host copy is freed before the open returns: the texture rung's own note gives th
 *   e rule for a record that reached the write - the bytes the upload read are already inside the imag
 *   e, and a record that kept them would be a record holding a second copy of every texture on the host.
 *   A font is a texture plus metrics, and the metrics are 32 bytes a glyph rather than dim*dim bytes, so
 *   the rows are the only thing worth keeping and the atlas is not.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud_font.h" /* Required for: this file's own contract - the record, the rows, the bounds and every rung declared above */
#include "xud.h" /* Required for: XudResult, XudHandle, XudFontData, XudGlyphInfo, XudGlyphAtlasRec, XUD_HANDLE_KIND_FONT - the public surface above */

#include <assert.h> /* Required for: static_assert, the bake's own invariants below */
#include <stdlib.h> /* Required for: calloc, free - the record, the rows, the atlas and the default set */

#include "xud_core.h" /* Required for: struct XudContext, whose handleTable/loader/device/physicalDevice/allocator the public pair reaches */
#include "xud_device.h" /* Required for: XudDevice, the handle every guard below checks exists */
#include "xud_handle.h" /* Required for: xud_handle_validate/alloc/free, xud_handle_record_get/set - the slot this record lives in */
#include "xud_loader.h" /* Required for: xud_loader_is_open, xud_loader_device_bound - the two guards every driver half runs */
#include "xud_shapes.h" /* Required for: xud_shapes_append_glyph_quad() - the one shape this rung's pen walk appends */
#include "xud_texture.h" /* Required for: XUD_TEXTURE_FILTER_LINEAR, XUD_TEXTURE_ADDRESS_CLAMP_TO_EDGE, XUD_TEXTURE_BYTES_PER_TEXEL, xud_texture_open/close - the atlas */

static_assert(XUD_FONT_ATLAS_BUDGET(XUD_FONT_ATLAS_MIN_DIM, XUD_FONT_MIN_PIXEL_SIZE) > 0u,
    "xud_font.h: the smallest atlas still holds at least one box, so the dim loop below starts inside its own budget rather than above it");
static_assert((XUD_FONT_ATLAS_MIN_DIM * XUD_FONT_ATLAS_MIN_DIM) <= XUD_TEXTURE_MAX_TEXELS,
    "xud_font.h: the first attempt is inside the texture rung's texel bound, so the loop below never asks the rung below for an extent it "
    "would refuse");

/*-----------------------------------------------------------------------------------------------
 * The description's own count, and the default set it asks for
 *-----------------------------------------------------------------------------------------------*/

static uint32_t xud_font_effective_count(const XudFontData* data) {
    if ((data == NULL) || (data->codepoints == NULL)) {
        return XUD_FONT_DEFAULT_COUNT;
    }
    return data->codepointCount;
}

/*-----------------------------------------------------------------------------------------------
 * The check - pure, and the first thing every entry point runs
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_font_check(const XudFontData* data, uint32_t* outGlyphCount) {
    uint32_t glyphCount = 0u;
    uint32_t i;

    if (outGlyphCount != NULL) {
        *outGlyphCount = 0u;
    }
    if ((data == NULL) || (data->fileData == NULL) || (data->fileBytes < XUD_FONT_MIN_FILE_BYTES)) {
        return XUD_INVALID_ARG;
    }
    if ((data->pixelSize < XUD_FONT_MIN_PIXEL_SIZE) || (data->pixelSize > XUD_FONT_MAX_PIXEL_SIZE)) {
        return XUD_INVALID_ARG;
    }
    if (data->codepoints == NULL) {
        if (data->codepointCount != 0u) {
            return XUD_INVALID_ARG;
        }
        glyphCount = XUD_FONT_DEFAULT_COUNT;
    } else {
        glyphCount = data->codepointCount;
        if ((glyphCount == 0u) || (glyphCount > XUD_FONT_MAX_GLYPHS)) {
            return XUD_INVALID_ARG;
        }
        for (i = 0u; i < glyphCount; i++) {
            const uint32_t codepoint = data->codepoints[i];
            if ((codepoint < XUD_FONT_MIN_CODEPOINT) || (codepoint > XUD_FONT_MAX_CODEPOINT)) {
                return XUD_INVALID_ARG;
            }
            if ((i > 0u) && (codepoint <= data->codepoints[i - 1u])) {
                return XUD_INVALID_ARG;
            }
        }
    }
    if (glyphCount > XUD_FONT_ATLAS_BUDGET(XUD_FONT_ATLAS_MAX_DIM, data->pixelSize)) {
        return XUD_INVALID_ARG;
    }
    if (outGlyphCount != NULL) {
        *outGlyphCount = glyphCount;
    }
    return XUD_OK;
}

XudResult xud_font_default_count(uint32_t* outFirst, uint32_t* outCount) {
    if ((outFirst == NULL) || (outCount == NULL)) {
        return XUD_INVALID_ARG;
    }
    *outFirst = XUD_FONT_DEFAULT_FIRST;
    *outCount = XUD_FONT_DEFAULT_COUNT;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The atlas's first attempt - pure
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_font_atlas_dim(uint32_t glyphCount, uint32_t pixelSize, uint32_t* outDim) {
    uint32_t dim = XUD_FONT_ATLAS_MIN_DIM;

    if ((outDim == NULL) || (glyphCount == 0u) || (glyphCount > XUD_FONT_MAX_GLYPHS)) {
        return XUD_INVALID_ARG;
    }
    if ((pixelSize < XUD_FONT_MIN_PIXEL_SIZE) || (pixelSize > XUD_FONT_MAX_PIXEL_SIZE)) {
        return XUD_INVALID_ARG;
    }
    *outDim = 0u;
    while ((dim < XUD_FONT_ATLAS_MAX_DIM) && (glyphCount > XUD_FONT_ATLAS_BUDGET(dim, pixelSize))) {
        dim *= 2u;
    }
    if (glyphCount > XUD_FONT_ATLAS_BUDGET(dim, pixelSize)) {
        return XUD_UNSUPPORTED;
    }
    *outDim = dim;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The bake - the only place in this tree that calls stb_truetype
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_font_result_from_stb(int packed, int fontOffset) {
    if ((fontOffset < 0) || (packed == 0)) {
        return XUD_UNSUPPORTED;
    }
    return XUD_OK;
}

XudResult xud_font_bake(const XudFontData* data, uint32_t dim, XudGlyphRow* outRows, uint8_t** outCoverage) {
    stbtt_fontinfo fontInfo;
    stbtt_pack_context packContext;
    stbtt_pack_range range;
    stbtt_packedchar* packed = NULL;
    int* codepoints = NULL;
    uint8_t* coverage = NULL;
    uint32_t glyphCount = 0u;
    uint32_t i;
    int offset = -1;
    int packedAnswer = 0;
    XudResult result = XUD_INTERNAL_ERROR;

    if (outCoverage == NULL) {
        return XUD_INVALID_ARG;
    }
    *outCoverage = NULL;
    if ((data == NULL) || (outRows == NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((dim < XUD_FONT_ATLAS_MIN_DIM) || (dim > XUD_FONT_ATLAS_MAX_DIM)) {
        return XUD_INVALID_ARG;
    }
    if ((data->fileData == NULL) || (data->fileBytes < XUD_FONT_MIN_FILE_BYTES)) {
        return XUD_INVALID_ARG;
    }
    if ((data->pixelSize < XUD_FONT_MIN_PIXEL_SIZE) || (data->pixelSize > XUD_FONT_MAX_PIXEL_SIZE)) {
        return XUD_INVALID_ARG;
    }
    glyphCount = xud_font_effective_count(data);
    if ((glyphCount == 0u) || (glyphCount > XUD_FONT_MAX_GLYPHS)) {
        return XUD_INVALID_ARG;
    }
    if (data->codepoints != NULL) {
        for (i = 0u; i < glyphCount; i++) {
            if ((data->codepoints[i] < XUD_FONT_MIN_CODEPOINT) || (data->codepoints[i] > XUD_FONT_MAX_CODEPOINT)) {
                return XUD_INVALID_ARG;
            }
        }
    }

    coverage = (uint8_t*)calloc((size_t)dim * (size_t)dim, 1u);
    codepoints = (int*)calloc((size_t)glyphCount, sizeof(int));
    packed = (stbtt_packedchar*)calloc((size_t)glyphCount, sizeof(stbtt_packedchar));
    if ((coverage == NULL) || (codepoints == NULL) || (packed == NULL)) {
        free(coverage);
        free(codepoints);
        free(packed);
        return XUD_OUT_OF_MEMORY;
    }
    for (i = 0u; i < glyphCount; i++) {
        codepoints[i] = (data->codepoints == NULL) ? (int)(XUD_FONT_DEFAULT_FIRST + i) : (int)data->codepoints[i];
    }

    /* The header read first, and the two answers stb gives for "this is not a font" are one refusal:
       the offset rung answers -1 for a buffer whose sfnt header it cannot find, and the init rung
       answers 0 for a header it can find but cannot walk. Neither is a code this library can tell a
       caller the difference between, so both are the same XUD_UNSUPPORTED the conversion rung below
       states.
    */
    offset = stbtt_GetFontOffsetForIndex((const unsigned char*)data->fileData, 0);
    if ((offset < 0) || (stbtt_InitFont(&fontInfo, (const unsigned char*)data->fileData, offset) == 0)) {
        free(coverage);
        free(codepoints);
        free(packed);
        return XUD_UNSUPPORTED;
    }

    /* The pack context is opened over the atlas buffer itself: stb's own note above PackBegin says th
       e pixels array is where the glyphs end up, and a context opened over a buffer this rung does not
       own would be a bake writing into a caller's bytes. The stride is 0 because the atlas is one tight
       square, and the padding is this rung's own - the clear space that keeps one glyph's bilinear fetc
       h from bleeding into its neighbour's.
    */
    if (stbtt_PackBegin(&packContext, coverage, (int)dim, (int)dim, 0, (int)XUD_FONT_ATLAS_PADDING, NULL) == 0) {
        free(coverage);
        free(codepoints);
        free(packed);
        return XUD_UNSUPPORTED;
    }
    range.font_size = (float)data->pixelSize;
    range.first_unicode_codepoint_in_range = 0;
    range.array_of_unicode_codepoints = codepoints;
    range.num_chars = (int)glyphCount;
    range.chardata_for_range = packed;
    packedAnswer = stbtt_PackFontRanges(&packContext, (const unsigned char*)data->fileData, 0, &range, 1);
    stbtt_PackEnd(&packContext);

    /* The pack's answer is the one bit that decides whether the caller above retries: 0 means "a rect o
       f this set did not fit this atlas", which is the description's fault rather than the driver's, and
       is the same XUD_UNSUPPORTED the texture rung gives for an extent its own bound refuses.
    */
    result = xud_font_result_from_stb(packedAnswer, offset);
    if (result != XUD_OK) {
        free(coverage);
        free(codepoints);
        free(packed);
        return result;
    }

    /* The packer's box becomes this library's row, and the two differences between the two shapes are
       the two this rung decides: stb's box is a pair of corners and can be empty (a codepoint with no
       outline, a space, packs as a zero-size rect), so the width is a clamped difference rather than a
       copy; and stb's pen numbers are already in the bake's own pixel space, so they are copied rather
       than scaled.
    */
    for (i = 0u; i < glyphCount; i++) {
        const stbtt_packedchar* glyph = &packed[i];
        outRows[i].codepoint = (uint32_t)codepoints[i];
        outRows[i].atlasX = (uint32_t)glyph->x0;
        outRows[i].atlasY = (uint32_t)glyph->y0;
        outRows[i].atlasWidth = (glyph->x1 > glyph->x0) ? (uint32_t)(glyph->x1 - glyph->x0) : 0u;
        outRows[i].atlasHeight = (glyph->y1 > glyph->y0) ? (uint32_t)(glyph->y1 - glyph->y0) : 0u;
        outRows[i].offsetX = glyph->xoff;
        outRows[i].offsetY = glyph->yoff;
        outRows[i].advanceX = glyph->xadvance;
    }
    free(codepoints);
    free(packed);
    *outCoverage = coverage;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The coverage-to-texel conversion - pure
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_font_atlas_rgba(uint8_t** outRgba, const uint8_t* coverage, uint32_t dim) {
    uint8_t* rgba = NULL;
    uint32_t i;

    if ((outRgba == NULL) || (coverage == NULL) || (dim == 0u)) {
        return XUD_INVALID_ARG;
    }
    *outRgba = NULL;
    rgba = (uint8_t*)calloc((size_t)dim * (size_t)dim * XUD_TEXTURE_BYTES_PER_TEXEL, 1u);
    if (rgba == NULL) {
        return XUD_OUT_OF_MEMORY;
    }
    for (i = 0u; i < (dim * dim); i++) {
        rgba[(i * 4u) + 0u] = XUD_FONT_INK_CHANNEL;
        rgba[(i * 4u) + 1u] = XUD_FONT_INK_CHANNEL;
        rgba[(i * 4u) + 2u] = XUD_FONT_INK_CHANNEL;
        rgba[(i * 4u) + 3u] = coverage[i];
    }
    *outRgba = rgba;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The lookup, the two metric reads and the measurement - pure
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_font_glyph_index(const XudGlyphRow* rows, uint32_t glyphCount, uint32_t codepoint, uint32_t* outIndex) {
    uint32_t low = 0u;
    uint32_t high = glyphCount;

    if ((outIndex == NULL) || (rows == NULL) || (glyphCount == 0u)) {
        return XUD_INVALID_ARG;
    }
    while (low < high) {
        const uint32_t middle = low + ((high - low) / 2u);
        if (rows[middle].codepoint < codepoint) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    if ((low < glyphCount) && (rows[low].codepoint == codepoint)) {
        *outIndex = low;
        return XUD_OK;
    }
    for (low = 0u; low < glyphCount; low++) {
        if (rows[low].codepoint == XUD_FONT_FALLBACK_CODEPOINT) {
            *outIndex = low;
            return XUD_OK;
        }
    }
    *outIndex = 0u;
    return XUD_OK;
}

XudResult xud_font_glyph_info(XudGlyphInfo* outInfo, const XudGlyphRow* rows, uint32_t glyphCount, uint32_t index) {
    if ((outInfo == NULL) || (rows == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (index >= glyphCount) {
        return XUD_ALREADY;
    }
    outInfo->value = rows[index].codepoint;
    outInfo->offsetX = rows[index].offsetX;
    outInfo->offsetY = rows[index].offsetY;
    outInfo->advanceX = rows[index].advanceX;
    return XUD_OK;
}

XudResult xud_font_glyph_atlas_rec(XudGlyphAtlasRec* outRec, const XudGlyphRow* rows, uint32_t glyphCount, uint32_t index) {
    if ((outRec == NULL) || (rows == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (index >= glyphCount) {
        return XUD_ALREADY;
    }
    outRec->x = (float)rows[index].atlasX;
    outRec->y = (float)rows[index].atlasY;
    outRec->width = (float)rows[index].atlasWidth;
    outRec->height = (float)rows[index].atlasHeight;
    return XUD_OK;
}

XudResult xud_font_measure(float* outWidth, const XudGlyphRow* rows, uint32_t glyphCount, uint32_t pixelSize, const uint32_t* text,
    uint32_t textCount, float requestedSize, float spacing) {
    float width = 0.0f;
    float scale = 0.0f;
    uint32_t i;

    if ((outWidth == NULL) || (rows == NULL) || (text == NULL) || (textCount == 0u)) {
        return XUD_INVALID_ARG;
    }
    if (pixelSize == 0u) {
        return XUD_UNSUPPORTED;
    }
    if (glyphCount == 0u) {
        *outWidth = 0.0f;
        return XUD_OK;
    }
    scale = (float)requestedSize / (float)pixelSize;
    for (i = 0u; i < textCount; i++) {
        uint32_t index = 0u;
        xud_font_glyph_index(rows, glyphCount, text[i], &index);
        width += (rows[index].advanceX * scale) + spacing;
    }
    *outWidth = width;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The pen walk - above the shapes rung, and the first real uv this tree writes
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_font_append_text(XudBatch* batch, const XudFontRecord* record, const uint32_t* text, uint32_t textCount, XudVector3 position,
    float pixelSize, float spacing, XudColor color) {
    XudGlyphRow row;
    XudVector3 corner0;
    XudVector3 corner1;
    XudVector2 uv0;
    XudVector2 uv1;
    float scale = 0.0f;
    float penX = 0.0f;
    float atlasWidth = 0.0f;
    float atlasHeight = 0.0f;
    XudResult result = XUD_INVALID_ARG;
    uint32_t index = 0u;
    uint32_t step = 0u;

    if ((batch == NULL) || (record == NULL) || (text == NULL) || (textCount == 0u)) {
        return XUD_INVALID_ARG;
    }

    /* The requested em height, in one comparison: a height of zero would scale every box to nothing
       and a negative one would flip it, and both are refusals rather than draws of a degenerate quad
       the shapes rung would refuse anyway - and the same comparison refuses a NaN, which is the reas
       on the sphere's note gives for a NaN radius, one rung up.
    */
    if (!(pixelSize > 0.0f)) {
        return XUD_INVALID_ARG;
    }

    /* The record's own numbers are the walk's divisors, so a record that never had them is a state xud_
       font_open() made rather than an argument a caller could fix, and it is reported rather than divided
       by: a uv that came out of a zero extent would be an inf in a float stream.
    */
    if ((record->pixelSize == 0u) || (record->glyphCount == 0u) || (record->glyphs == NULL) || (record->atlasWidth == 0u)
        || (record->atlasHeight == 0u)) {
        return XUD_INTERNAL_ERROR;
    }

    /* The scale is the divide the measure does, read from the other side: one atlas, many sizes, and
       the requested height is in the shape's own space rather than in the bake's, which is what lets a
       caller ask for an em height in clip units and get a width back in the same units.
    */
    scale = pixelSize / (float)record->pixelSize;
    atlasWidth = (float)record->atlasWidth;
    atlasHeight = (float)record->atlasHeight;
    penX = position.x;

    for (step = 0u; step < textCount; step++) {
        result = xud_font_glyph_index(record->glyphs, record->glyphCount, text[step], &index);
        if (result != XUD_OK) {
            return result;
        }
        row = record->glyphs[index];

        /* The two boxes, spelled the way the shapes rung's pairing note spells them: corner0 is the
           corner with the SMALLER x and the LARGER y, which is the atlas box's own top-down spelling
           turned into this rung's y-up space, and uv0 is the same corner of the atlas box rather than
           the diagonally opposite one - which is why the divide below needs no flip at any glyph. The
           top edge is the pen's y MINUS the row's offsetY because offsetY is the pen's step to the box's
           top and is negative above the baseline in stb's own y-down bake space.
        */
        corner0.x = penX + (row.offsetX * scale);
        corner0.y = position.y - (row.offsetY * scale);
        corner0.z = position.z;
        corner1.x = corner0.x + ((float)row.atlasWidth * scale);
        corner1.y = corner0.y - ((float)row.atlasHeight * scale);
        corner1.z = position.z;
        uv0.x = (float)row.atlasX / atlasWidth;
        uv0.y = (float)row.atlasY / atlasHeight;
        uv1.x = (float)(row.atlasX + row.atlasWidth) / atlasWidth;
        uv1.y = (float)(row.atlasY + row.atlasHeight) / atlasHeight;

        /* A codepoint with no outline - a space - has a box of zero width and still moves the pen: the
           append is skipped and the advance is not, which is the answer the rows state rather than an
           absent row, and skipping both would run two words into one. The box check is on the texel
           counts rather than on the scaled floats so a subpixel-scaled box is drawn rather than dropped.
        */
        if ((row.atlasWidth > 0u) && (row.atlasHeight > 0u)) {
            result = xud_shapes_append_glyph_quad(batch, corner0, corner1, uv0, uv1, color);
            if (result != XUD_OK) {
                return result;
            }
        }

        /* The same step xud_font_measure() sums, after EVERY glyph including the last one, which is what
           makes a measure and a draw of one string agree about where the pen ends.
        */
        penX += (row.advanceX * scale) + spacing;
    }

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The accessor - pure
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_font_get(const XudHandleTable* table, XudHandle handle, const XudFontRecord** outRecord) {
    void* slotRecord = NULL;
    XudResult result = XUD_INTERNAL_ERROR;

    if ((outRecord == NULL) || (table == NULL)) {
        return XUD_INVALID_ARG;
    }
    *outRecord = NULL;
    result = xud_handle_validate(table, handle, XUD_HANDLE_KIND_FONT);
    if (result != XUD_OK) {
        return result;
    }
    if (xud_handle_record_get(table, XUD_HANDLE_KIND_FONT, handle.index, &slotRecord) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    if (slotRecord == NULL) {
        return XUD_INVALID_ARG;
    }
    *outRecord = (const XudFontRecord*)slotRecord;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The open - the whole rung in one call
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_font_open(XudHandle* outHandle, XudHandleTable* table, XudLoader* loader, const XudDevice* device, const XudDeviceCaps* caps,
    XudAllocator* alloc, const XudFontData* data) {
    XudFontRecord* record = NULL;
    XudGlyphRow* rows = NULL;
    uint8_t* coverage = NULL;
    uint8_t* rgba = NULL;
    XudTextureData atlas;
    XudHandle atlasHandle;
    XudHandle fontHandle;
    XudResult result = XUD_INTERNAL_ERROR;
    uint32_t glyphCount = 0u;
    uint32_t dim = 0u;
    uint32_t attempt = 0u;

    if ((outHandle == NULL) || (table == NULL) || (loader == NULL) || (device == NULL) || (caps == NULL) || (alloc == NULL)
        || (data == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (!xud_loader_is_open(loader)) {
        return XUD_UNSUPPORTED;
    }
    if (device->device == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }
    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }

    /* The argument check and the dim rung run before a slot is spent, for the rule every rung in this t
       ree states: a refusal leaves no half-font behind. The slot, the record and the rows are allocated o
       nly once the description is known to be one this rung can lay out.
    */
    result = xud_font_check(data, &glyphCount);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_font_atlas_dim(glyphCount, data->pixelSize, &dim);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_handle_alloc(outHandle, table, XUD_HANDLE_KIND_FONT);
    if (result != XUD_OK) {
        return result;
    }
    fontHandle = *outHandle;
    record = (XudFontRecord*)calloc(1u, sizeof(XudFontRecord));
    if (record == NULL) {
        (void)xud_handle_free(table, fontHandle);
        *outHandle = (XudHandle) { 0u, 0u, XUD_HANDLE_KIND_NONE };
        return XUD_OUT_OF_MEMORY;
    }
    record->atlas = (XudHandle) { 0u, 0u, XUD_HANDLE_KIND_NONE };
    record->pixelSize = data->pixelSize;
    record->padding = XUD_FONT_ATLAS_PADDING;
    record->glyphCount = glyphCount;
    record->atlasWidth = 0u;
    record->atlasHeight = 0u;
    record->glyphs = NULL;
    rows = (XudGlyphRow*)calloc(glyphCount, sizeof(XudGlyphRow));
    if (rows == NULL) {
        free(record);
        (void)xud_handle_free(table, fontHandle);
        *outHandle = (XudHandle) { 0u, 0u, XUD_HANDLE_KIND_NONE };
        return XUD_OUT_OF_MEMORY;
    }

    /* The retry loop, and the reason it is a loop rather than one call is stb's own note: its packing i
       s "very crappy", so the budget above is a budget and not a fit. Every attempt is a fresh atlas buff
       er, freed whether it was accepted or not - the accepted one is converted and uploaded below, and the
       bytes the upload reads never outlive the open (the texture rung's own rule).
    */
    for (attempt = dim; attempt <= XUD_FONT_ATLAS_MAX_DIM; attempt *= 2u) {
        result = xud_font_bake(data, attempt, rows, &coverage);
        if (result == XUD_OK) {
            dim = attempt;
            break;
        }
        free(coverage);
        coverage = NULL;
        if (result != XUD_UNSUPPORTED) {
            free(rows);
            free(record);
            (void)xud_handle_free(table, fontHandle);
            *outHandle = (XudHandle) { 0u, 0u, XUD_HANDLE_KIND_NONE };
            return result;
        }
    }
    if (coverage == NULL) {
        free(rows);
        free(record);
        (void)xud_handle_free(table, fontHandle);
        *outHandle = (XudHandle) { 0u, 0u, XUD_HANDLE_KIND_NONE };
        return XUD_UNSUPPORTED;
    }

    result = xud_font_atlas_rgba(&rgba, coverage, dim);
    free(coverage);
    if (result != XUD_OK) {
        free(rows);
        free(record);
        (void)xud_handle_free(table, fontHandle);
        *outHandle = (XudHandle) { 0u, 0u, XUD_HANDLE_KIND_NONE };
        return result;
    }

    /* The atlas is handed down as one texture description rather than uploaded here: the texture rung a
       bove owns the staging arena, the per-mip copy, the layout transition, the sampler and the view, and a
       second upload path in this rung would be a second copy of that path. The two sampling tokens are this
       rung's decision rather than the caller's: a glyph's coverage is a gradient, so a NEAREST filter turns e
       very edge into a stair, and an atlas is not a tileable surface, so a REPEAT would bleed the glyph at the f
       ar edge into the glyph at this edge.
    */
    atlas.width = dim;
    atlas.height = dim;
    atlas.rgba = rgba;
    atlas.filter = XUD_TEXTURE_FILTER_LINEAR;
    atlas.addressMode = XUD_TEXTURE_ADDRESS_CLAMP_TO_EDGE;
    result = xud_texture_open(&atlasHandle, table, loader, device, caps, alloc, &atlas);
    free(rgba);
    if (result != XUD_OK) {
        free(rows);
        free(record);
        (void)xud_handle_free(table, fontHandle);
        *outHandle = (XudHandle) { 0u, 0u, XUD_HANDLE_KIND_NONE };
        return result;
    }

    record->atlas = atlasHandle;
    record->glyphs = rows;
    record->atlasWidth = dim;
    record->atlasHeight = dim;
    if (xud_handle_record_set(table, XUD_HANDLE_KIND_FONT, fontHandle.index, record) != XUD_OK) {
        (void)xud_texture_close(table, atlasHandle, loader, device, alloc);
        free(rows);
        free(record);
        (void)xud_handle_free(table, fontHandle);
        *outHandle = (XudHandle) { 0u, 0u, XUD_HANDLE_KIND_NONE };
        return XUD_INVALID_ARG;
    }
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The close - everything the record owns, in the reverse of the order above
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_font_close(XudHandleTable* table, XudHandle handle, XudLoader* loader, const XudDevice* device, XudAllocator* alloc) {
    XudFontRecord* record = NULL;
    XudHandle atlasHandle;
    XudResult result = XUD_INTERNAL_ERROR;

    if ((table == NULL) || (loader == NULL) || (device == NULL) || (alloc == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (!xud_loader_is_open(loader)) {
        return XUD_UNSUPPORTED;
    }
    if (device->device == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }
    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }
    if (xud_handle_validate(table, handle, XUD_HANDLE_KIND_FONT) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    if (xud_handle_record_get(table, XUD_HANDLE_KIND_FONT, handle.index, (void**)&record) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    if (record == NULL) {
        return XUD_INVALID_ARG;
    }

    /* The atlas first, and through the TEXTURE rung rather than by destroying anything here: this rung o
       wns no driver object of its own, and the wait-idle guard that belongs to the atlas is the texture rung'
       s - a guard in this rung would be a second copy of the rule the rung below already runs, and it would run
       it for an object this rung does not hold.
    */
    atlasHandle = record->atlas;
    if (atlasHandle.kind == XUD_HANDLE_KIND_TEXTURE) {
        result = xud_texture_close(table, atlasHandle, loader, device, alloc);
        if (result != XUD_OK) {
            return result;
        }
    }

    /* Cleared BEFORE the free and before the slot goes free, the handle layer's own rule: xud_handle_free()
       leaves the record field as it was, so a free slot can otherwise still own a record, and then this rung'
       s teardown and the table's would both destroy it.
    */
    (void)xud_handle_record_set(table, XUD_HANDLE_KIND_FONT, handle.index, NULL);
    free(record->glyphs);
    free(record);
    (void)xud_handle_free(table, handle);
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The teardown rung - above xud_texture_close_all()
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_font_close_all(XudHandleTable* table, XudLoader* loader, const XudDevice* device, XudAllocator* alloc, uint32_t* outClosed) {
    XudFontRecord* record = NULL;
    XudHandle atlasHandle;
    XudResult first = XUD_OK;
    uint32_t index = 0u;

    if ((table == NULL) || (loader == NULL) || (device == NULL) || (alloc == NULL) || (outClosed == NULL)) {
        return XUD_INVALID_ARG;
    }
    *outClosed = 0u;
    if (!xud_loader_is_open(loader)) {
        return XUD_UNSUPPORTED;
    }
    if (device->device == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }
    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }
    if (table->slots[XUD_HANDLE_KIND_FONT] == NULL) {
        return XUD_OK;
    }

    /* Every row of the FONT array is walked, live or free: xud_handle_free() leaves the record field as it w
       as, so a slot a caller released still owns its rows and its atlas until this rung clears it. The walk is
       bounded by the array's own high mark rather than by its capacity, for the texture rung's reason.
    */
    for (index = 0u; index < table->slotCount[XUD_HANDLE_KIND_FONT]; index++) {
        if (xud_handle_record_get(table, XUD_HANDLE_KIND_FONT, index, (void**)&record) != XUD_OK) {
            return XUD_INVALID_ARG;
        }
        if (record == NULL) {
            continue;
        }
        atlasHandle = ((XudFontRecord*)record)->atlas;
        if (atlasHandle.kind == XUD_HANDLE_KIND_TEXTURE) {
            first = xud_texture_close(table, atlasHandle, loader, device, alloc);
            if (first != XUD_OK) {
                return first;
            }
        }
        (void)xud_handle_record_set(table, XUD_HANDLE_KIND_FONT, index, NULL);
        free(((XudFontRecord*)record)->glyphs);
        free(record);
        (*outClosed)++;
    }
    return first;
}

/*-----------------------------------------------------------------------------------------------
 * The public pair and the public metric queries - xud.h's surface, one bundle in and the rung above
 *   read out of it
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_load_font(XudHandle* outHandle, XudContext* context, const XudFontData* font) {
    if ((outHandle == NULL) || (context == NULL) || (font == NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((context->handleTable == NULL) || (context->loader == NULL) || (context->device == NULL) || (context->physicalDevice == NULL)
        || (context->allocator == NULL)) {
        return XUD_UNSUPPORTED;
    }
    return xud_font_open(
        outHandle, context->handleTable, context->loader, context->device, &context->physicalDevice->caps, context->allocator, font);
}

XudResult xud_close_font(XudContext* context, XudHandle handle) {
    if (context == NULL) {
        return XUD_INVALID_ARG;
    }
    if ((context->handleTable == NULL) || (context->loader == NULL) || (context->device == NULL) || (context->allocator == NULL)) {
        return XUD_UNSUPPORTED;
    }
    return xud_font_close(context->handleTable, handle, context->loader, context->device, context->allocator);
}

XudResult xud_measure_text(
    float* outWidth, XudContext* context, XudHandle handle, const uint32_t* text, uint32_t textCount, float pixelSize, float spacing) {
    const XudFontRecord* record = NULL;
    XudResult result = XUD_INTERNAL_ERROR;

    if ((outWidth == NULL) || (context == NULL) || (text == NULL) || (textCount == 0u)) {
        return XUD_INVALID_ARG;
    }
    *outWidth = 0.0f;
    result = xud_font_get(context->handleTable, handle, &record);
    if (result != XUD_OK) {
        return result;
    }
    return xud_font_measure(outWidth, record->glyphs, record->glyphCount, record->pixelSize, text, textCount, pixelSize, spacing);
}

XudResult xud_get_glyph_index(uint32_t* outIndex, XudContext* context, XudHandle handle, uint32_t codepoint) {
    const XudFontRecord* record = NULL;
    XudResult result = XUD_INTERNAL_ERROR;

    if ((outIndex == NULL) || (context == NULL)) {
        return XUD_INVALID_ARG;
    }
    *outIndex = 0u;
    result = xud_font_get(context->handleTable, handle, &record);
    if (result != XUD_OK) {
        return result;
    }
    return xud_font_glyph_index(record->glyphs, record->glyphCount, codepoint, outIndex);
}

XudResult xud_get_glyph_info(XudGlyphInfo* outInfo, XudContext* context, XudHandle handle, uint32_t codepoint) {
    const XudFontRecord* record = NULL;
    XudResult result = XUD_INTERNAL_ERROR;
    uint32_t index = 0u;

    if ((outInfo == NULL) || (context == NULL)) {
        return XUD_INVALID_ARG;
    }
    result = xud_font_get(context->handleTable, handle, &record);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_font_glyph_index(record->glyphs, record->glyphCount, codepoint, &index);
    if (result != XUD_OK) {
        return result;
    }
    return xud_font_glyph_info(outInfo, record->glyphs, record->glyphCount, index);
}

/*-----------------------------------------------------------------------------------------------
 * The one guard every public draw above this rung needs
 *-----------------------------------------------------------------------------------------------*/

/* The guard is one function rather than one block per public call, the same shape xud_shapes.c gives
   the drawing API above it: a text draw reaches the same two fields of the same owner and answers the
   same two codes as every other draw above the renderer does, and it is file-local because it is not
   the layer's contract - the contract is the walk and the public pair, and this is the half they share.
*/
static XudResult xud_font_context_batch(XudBatch** outBatch, XudContext* context) {
    if (context == NULL) {
        return XUD_INVALID_ARG;
    }
    if ((context->renderer == NULL) || (context->renderer->batch == NULL)) {
        return XUD_UNSUPPORTED;
    }

    *outBatch = context->renderer->batch;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The public text pair - raylib's DrawText/DrawTextEx above the pen walk
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_draw_text(
    XudContext* context, XudHandle handle, const uint32_t* text, uint32_t textCount, XudVector3 position, float pixelSize, XudColor color) {
    XudBatch* batch = NULL;
    const XudFontRecord* record = NULL;
    XudResult result = XUD_INVALID_ARG;

    result = xud_font_context_batch(&batch, context);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_font_get(context->handleTable, handle, &record);
    if (result != XUD_OK) {
        return result;
    }

    return xud_font_append_text(batch, record, text, textCount, position, pixelSize, 0.0f, color);
}

XudResult xud_draw_text_ex(XudContext* context, XudHandle handle, const uint32_t* text, uint32_t textCount, XudVector3 position,
    float pixelSize, float spacing, XudColor color) {
    XudBatch* batch = NULL;
    const XudFontRecord* record = NULL;
    XudResult result = XUD_INVALID_ARG;

    result = xud_font_context_batch(&batch, context);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_font_get(context->handleTable, handle, &record);
    if (result != XUD_OK) {
        return result;
    }

    return xud_font_append_text(batch, record, text, textCount, position, pixelSize, spacing, color);
}

XudResult xud_get_glyph_atlas_rec(XudGlyphAtlasRec* outRec, XudContext* context, XudHandle handle, uint32_t codepoint) {
    const XudFontRecord* record = NULL;
    XudResult result = XUD_INTERNAL_ERROR;
    uint32_t index = 0u;

    if ((outRec == NULL) || (context == NULL)) {
        return XUD_INVALID_ARG;
    }
    result = xud_font_get(context->handleTable, handle, &record);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_font_glyph_index(record->glyphs, record->glyphCount, codepoint, &index);
    if (result != XUD_OK) {
        return result;
    }
    return xud_font_glyph_atlas_rec(outRec, record->glyphs, record->glyphCount, index);
}
