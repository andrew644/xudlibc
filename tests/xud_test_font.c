/***********************************************************************************************
 *
 *   xud_test_font.c - the font rung and the public font API above it
 *
 *   Owns : the description's argument check and its box budget, the default set's own bounds, the
 *          atlas's first attempt, stb's three bake steps run over a real font file, the coverage-to
 *          texel conversion, the row lookup and its fallback, the two metric reads, the measuremen
 *          t and the scale it divides by, the code stb's own answers turn into this library's, and
 *          the public pair xud_load_font()/xud_close_font() plus the four public queries - and not
 *          hing else: the handle layer's own validation order is the handle area's, the atlas's up
 *          load (the staging arena, the copy, the layout transition, the sampler and the view) is t
 *          he texture area's, the allocator's block discipline is the allocator area's, and the te
 *          ardown rung xud_close_context() runs above the texture kind's is the core area's.
 *
 *   Layer: tests, above the texture area and below the core area. The pure half runs with no driver
 *   present at all - a bake is stb writing into a heap buffer, so the atlas, the rows and the texe
 *   l conversion are all answerable with no device in sight. The GPU half runs the same public pair
 *   against a real context, which is where the atlas the load hands the texture rung is observable
 *   at all.
 *
 *   What the checks are stated against: the budget's arithmetic rather than the packer's answer fo
 *   r the dim (the packer's packing is "very crappy", per its own note, so the budget is a bound an
 *   d not a fit, and the one thing stated from a real bake is that a box is inside the atlas rather
 *   than where in the atlas it is), the set's own bounds for the default set, the codepoint array's
 *   ordering and the fallback row for the lookup, and the metrics of ONE glyph of a real font file
 *   for the measurement - so the pure half's numbers are the font file's own answers rather than a
 *   table someone copied, and the GPU half's are the SAME numbers read back through the public API,
 *   which is what makes the two halves one bake rather than two hopes.
 *
 *   Deterministic: one fixed font file, one fixed em height, one fixed codepoint set, one fixed at
 *   las extent for the pure bake, no time, no randomness. The font file is read rather than embedded
 *   for the reason the model area reads its document from disk, and a run on a machine that has no
 *   such file prints a skip note rather than passing silently - a skipped half is a line on stdout, n
 *   ot nothing.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include <stdio.h> /* Required for: fopen, fread, fclose, ftell and printf, the one font file both halves read */
#include <stdlib.h> /* Required for: malloc and free, the buffer the file is read into and the atlas the bake writes */
#include <string.h> /* Required for: memset, the descriptions and the rows before they are filled */

#include "xud.h" /* Required for: XudResult, XudHandle, XudContext, XudFontData, XudGlyphInfo, XudGlyphAtlasRec and the public font API */
#include "xud_core.h" /* Required for: struct XudContext, whose handleTable/loader/device/allocator the GPU half reaches */
#include "xud_font.h" /* Required for: XudFontRecord, XudGlyphRow and the rung's own check/dim/bake/convert/lookup/measure halves */
#include "xud_handle.h" /* Required for: XudHandleTable, xud_handle_table_live_count - the slot this record lives in, and the TEXTURE slot it owns */
#include "xud_test_harness.h" /* Required for: xud_check, xud_gpu_tests_enabled, the area entry points */

/*-----------------------------------------------------------------------------------------------
 * The one font file every "the font is fine" check below is the same font seen through a
 * different refusal, and the one em height and one codepoint set every bake is stated against.
 * The file is read rather than embedded because a real sfnt is two megabytes of tables, and it is
 * read from the gitignored resources/ dir the repo keeps its test assets in rather than from a
 * system font dir, so the bake below is stated against one file the repo itself carries rather
 * than against whichever font collection this machine happens to ship. The height is 16 because it
 * is the smallest height whose boxes the default set's 95 glyphs fit at the dim the dim rung below
 * answers, so the pure bake and the GPU bake start at the same attempt and cannot disagree about
 * the metrics.
 *-----------------------------------------------------------------------------------------------*/

#define XUD_TEST_FONT_PATH "resources/fonts/NotoSansTC-Regular.ttf"
#define XUD_TEST_FONT_SIZE 16u
#define XUD_TEST_ATLAS_DIM 256u
#define XUD_TEST_GLYPH_FIRST XUD_FONT_DEFAULT_FIRST
#define XUD_TEST_GLYPH_COUNT XUD_FONT_DEFAULT_COUNT

static uint8_t* xudTestFontFile = NULL;
static uint32_t xudTestFontBytes = 0u;

static int xud_test_font_file_read(void) {
    FILE* file = NULL;
    long size = 0L;
    uint8_t* buffer = NULL;

    file = fopen(XUD_TEST_FONT_PATH, "rb");
    if (file == NULL) {
        return 0;
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        (void)fclose(file);
        return 0;
    }
    size = ftell(file);
    if (size <= 0L) {
        (void)fclose(file);
        return 0;
    }
    rewind(file);
    buffer = (uint8_t*)malloc((size_t)size);
    if (buffer == NULL) {
        (void)fclose(file);
        return 0;
    }
    if (fread(buffer, 1u, (size_t)size, file) != (size_t)size) {
        free(buffer);
        (void)fclose(file);
        return 0;
    }
    (void)fclose(file);
    xudTestFontFile = buffer;
    xudTestFontBytes = (uint32_t)size;
    return 1;
}

static void xud_test_font_file_drop(void) {
    free(xudTestFontFile);
    xudTestFontFile = NULL;
    xudTestFontBytes = 0u;
}

/*-----------------------------------------------------------------------------------------------
 * The description's argument check, its box budget and the default set's bounds
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_font_check(void) {
    XudFontData data;
    XudFontData bad;
    uint32_t many[XUD_FONT_MAX_GLYPHS];
    const uint32_t ordered[3] = { 65u, 66u, 67u };
    const uint32_t unordered[2] = { 66u, 65u };
    const uint32_t doubled[2] = { 65u, 65u };
    const uint32_t tooLow[1] = { 9u };
    const uint32_t tooHigh[1] = { 0x110000u };
    uint32_t answered = 0u;
    uint32_t i;

    memset(&data, 0, sizeof(data));
    data.fileData = (const uint8_t*)"0123456789ab";
    data.fileBytes = XUD_FONT_MIN_FILE_BYTES;
    data.pixelSize = XUD_TEST_FONT_SIZE;
    data.codepoints = NULL;
    data.codepointCount = 0u;

    xud_check(xud_font_check(NULL, &answered) == XUD_INVALID_ARG, "check-null-description",
        "a description with nothing in it is refused before a slot is spent");
    xud_check(xud_font_check(&data, NULL) == XUD_OK, "check-count-is-optional",
        "and the count is optional, so a caller that does not want it is not refused for wanting nothing");

    bad = data;
    bad.fileData = NULL;
    xud_check(xud_font_check(&bad, &answered) == XUD_INVALID_ARG, "check-no-bytes",
        "a font with no bytes is not a font with a default, it is a description with no font in it");

    bad = data;
    bad.fileBytes = XUD_FONT_MIN_FILE_BYTES - 1u;
    xud_check(xud_font_check(&bad, &answered) == XUD_INVALID_ARG, "check-short-header",
        "and bytes shorter than sfnt's own shortest header are refused here rather than read out of range below");

    bad = data;
    bad.pixelSize = XUD_FONT_MIN_PIXEL_SIZE - 1u;
    xud_check(xud_font_check(&bad, &answered) == XUD_INVALID_ARG, "check-size-below-floor",
        "an em height below this rung's floor is a description this rung cannot lay out");
    bad.pixelSize = XUD_FONT_MAX_PIXEL_SIZE + 1u;
    xud_check(xud_font_check(&bad, &answered) == XUD_INVALID_ARG, "check-size-above-ceiling", "and so is one above its ceiling");

    bad = data;
    bad.codepoints = ordered;
    bad.codepointCount = 0u;
    xud_check(xud_font_check(&bad, &answered) == XUD_INVALID_ARG, "check-array-without-count",
        "an array named with a count of zero is refused: the default set is a NULL array, and an array is a set");

    bad = data;
    bad.codepoints = unordered;
    bad.codepointCount = 2u;
    xud_check(xud_font_check(&bad, &answered) == XUD_INVALID_ARG, "check-unsorted",
        "an array that is not ascending is refused rather than sorted behind the caller's back");
    bad.codepoints = doubled;
    xud_check(xud_font_check(&bad, &answered) == XUD_INVALID_ARG, "check-duplicate",
        "and so is one that repeats a codepoint, which would be two rows one lookup cannot tell apart");

    bad = data;
    bad.codepoints = tooLow;
    bad.codepointCount = 1u;
    xud_check(xud_font_check(&bad, &answered) == XUD_INVALID_ARG, "check-codepoint-below",
        "a codepoint below the admitted range has no glyph to see");
    bad.codepoints = tooHigh;
    xud_check(xud_font_check(&bad, &answered) == XUD_INVALID_ARG, "check-codepoint-above",
        "and above Unicode's own top stb's int codepoints cannot name one");

    bad = data;
    bad.codepoints = ordered;
    bad.codepointCount = XUD_FONT_MAX_GLYPHS + 1u;
    xud_check(xud_font_check(&bad, &answered) == XUD_INVALID_ARG, "check-count-above",
        "a set past this rung's row bound is refused before the budget's own arithmetic runs");

    /* The box budget at its two sides: the largest atlas this rung will try holds 111 boxes of the
       largest size this rung admits, so 111 codepoints at that size is a description this rung can
       lay out and 112 is one it cannot - and the refusal is stated before anything is spent rather
       than after the last retry has failed.
    */
    for (i = 0u; i < XUD_FONT_MAX_GLYPHS; i++) {
        many[i] = XUD_FONT_MIN_CODEPOINT + i;
    }
    bad = data;
    bad.codepoints = many;
    bad.codepointCount = XUD_FONT_ATLAS_BUDGET(XUD_FONT_ATLAS_MAX_DIM, XUD_FONT_MAX_PIXEL_SIZE);
    bad.pixelSize = XUD_FONT_MAX_PIXEL_SIZE;
    xud_check(xud_font_check(&bad, &answered) == XUD_OK, "check-budget-exact",
        "a set that exactly fills the largest atlas's box budget is accepted");
    xud_check(answered == bad.codepointCount, "check-budget-exact-count",
        "and the count answered is the description's own rather than the budget's");
    bad.codepointCount = XUD_FONT_ATLAS_BUDGET(XUD_FONT_ATLAS_MAX_DIM, XUD_FONT_MAX_PIXEL_SIZE) + 1u;
    xud_check(xud_font_check(&bad, &answered) == XUD_INVALID_ARG, "check-budget-above",
        "one box more than the largest atlas can hold is refused, and the answer is the caller's description rather than this device's");
    xud_check(answered == 0u, "check-budget-refusal-clears", "and the refused count is cleared rather than left half-written");

    xud_check(xud_font_check(&data, &answered) == XUD_OK, "check-default-set",
        "the default set - a NULL array with a zero count - is a description this rung can lay out");
    xud_check(answered == XUD_FONT_DEFAULT_COUNT, "check-default-count-answered",
        "and the count it answers is the default set's own length, which is the number the rows below are sized by");

    data.codepoints = ordered;
    data.codepointCount = 3u;
    xud_check(xud_font_check(&data, &answered) == XUD_OK, "check-named-set",
        "a named set is accepted, and the array is read once here rather than kept");
    xud_check(answered == 3u, "check-named-count-answered", "and the count answered is the description's own");
}

static void xud_test_font_default(void) {
    uint32_t first = 0u;
    uint32_t count = 0u;

    xud_check(xud_font_default_count(&first, &count) == XUD_OK, "default-query",
        "the default set's bounds are answerable without a description at all");
    xud_check(first == XUD_FONT_DEFAULT_FIRST, "default-first", "the set starts at the space");
    xud_check(count == XUD_FONT_DEFAULT_COUNT, "default-count",
        "and ends at the tilde, whose span is the count - so the two bounds and the count are one fact");
    xud_check(
        xud_font_default_count(NULL, &count) == XUD_INVALID_ARG, "default-null-out", "and a bound with nowhere to be written is refused");
}

/*-----------------------------------------------------------------------------------------------
 * The atlas's first attempt - the budget's arithmetic, not the packer's answer
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_font_dim(void) {
    uint32_t dim = 0u;

    xud_check(xud_font_atlas_dim(XUD_TEST_GLYPH_COUNT, XUD_TEST_FONT_SIZE, &dim) == XUD_OK, "dim-default",
        "the default set at this height has a first attempt");
    xud_check(dim == XUD_TEST_ATLAS_DIM, "dim-default-value",
        "and it is 256: 95 boxes of 18x18 do not fit 128 squared and do fit 256 squared, which is the doubling sequence's stop rather than "
        "a hoped number");

    xud_check(xud_font_atlas_dim(1u, XUD_FONT_MIN_PIXEL_SIZE, &dim) == XUD_OK, "dim-one-glyph",
        "one glyph of the smallest size has a first attempt");
    xud_check(dim == XUD_FONT_ATLAS_MIN_DIM, "dim-one-glyph-value",
        "and it is the floor, so the common case costs one attempt rather than a doubling walk");

    xud_check(xud_font_atlas_dim(XUD_FONT_MAX_GLYPHS, XUD_FONT_MAX_PIXEL_SIZE, &dim) == XUD_UNSUPPORTED, "dim-no-atlas",
        "a set even the ceiling cannot hold is refused by the dim rung as well as by the check above");
    xud_check(dim == 0u, "dim-refusal-clears", "and the refused extent is cleared rather than left half-doubled");

    xud_check(xud_font_atlas_dim(0u, XUD_TEST_FONT_SIZE, &dim) == XUD_INVALID_ARG, "dim-empty-set", "a set with no glyphs has no atlas");
    xud_check(xud_font_atlas_dim(XUD_FONT_MAX_GLYPHS + 1u, XUD_TEST_FONT_SIZE, &dim) == XUD_INVALID_ARG, "dim-count-above",
        "and a count past the row bound is refused rather than multiplied into the budget");
    xud_check(xud_font_atlas_dim(XUD_TEST_GLYPH_COUNT, XUD_FONT_MIN_PIXEL_SIZE - 1u, &dim) == XUD_INVALID_ARG, "dim-size-below",
        "and a size outside the range is refused rather than squared into the box");
    xud_check(xud_font_atlas_dim(XUD_TEST_GLYPH_COUNT, XUD_TEST_FONT_SIZE, NULL) == XUD_INVALID_ARG, "dim-null-out",
        "and an extent with nowhere to be written is refused");
}

static void xud_test_font_stb(void) {
    xud_check(xud_font_result_from_stb(1, 0) == XUD_OK, "stb-ok", "the pack's success answer and a found header are this library's OK");
    xud_check(xud_font_result_from_stb(0, 0) == XUD_UNSUPPORTED, "stb-does-not-fit",
        "and a rect that did not pack is unsupported rather than a driver's refusal - it is the description's fault");
    xud_check(xud_font_result_from_stb(1, -1) == XUD_UNSUPPORTED, "stb-not-a-font",
        "and a header stb cannot find is unsupported too, which is one answer for a buffer that is not a font at all");
}

/*-----------------------------------------------------------------------------------------------
 * The bake over a real font file, the conversion, the lookup and the measurement
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_font_bake_pure(void) {
    XudFontData data;
    XudGlyphRow rows[XUD_TEST_GLYPH_COUNT];
    XudGlyphInfo info;
    XudGlyphAtlasRec rec;
    uint8_t* coverage = NULL;
    uint8_t* rgba = NULL;
    uint32_t i;
    uint32_t ink = 0u;
    uint32_t index = 0u;
    uint32_t letterIndex = 0u;
    uint32_t fallbackIndex = 0u;
    float width = 0.0f;
    const uint32_t pair[2] = { 65u, 66u };
    const uint32_t oneChar[1] = { 65u };

    memset(rows, 0, sizeof(rows));
    memset(&data, 0, sizeof(data));
    data.fileData = xudTestFontFile;
    data.fileBytes = xudTestFontBytes;
    data.pixelSize = XUD_TEST_FONT_SIZE;
    data.codepoints = NULL;
    data.codepointCount = 0u;

    xud_check(xud_font_bake(&data, XUD_TEST_ATLAS_DIM, rows, &coverage) == XUD_OK, "bake-real-font",
        "the default set baked at the dim the dim rung answers is a set stb's packer accepts");
    if (coverage == NULL) {
        printf("note: the bake produced no atlas, so the checks below it are skipped\n");
        return;
    }

    for (i = 0u; i < XUD_TEST_GLYPH_COUNT; i++) {
        if (rows[i].codepoint != (XUD_TEST_GLYPH_FIRST + i)) {
            break;
        }
    }
    xud_check(i == XUD_TEST_GLYPH_COUNT, "bake-rows-are-the-set",
        "every row is the codepoint the description named at that index, so the rows and the array cannot disagree");

    xud_check(xud_font_glyph_index(rows, XUD_TEST_GLYPH_COUNT, 65u, &letterIndex) == XUD_OK, "lookup-letter-query",
        "a codepoint the set covers is found");
    xud_check(letterIndex == (65u - XUD_TEST_GLYPH_FIRST), "lookup-letter-value",
        "and found by the search rather than by a scan - the index is its position in the sorted set");
    xud_check(xud_font_glyph_index(rows, XUD_TEST_GLYPH_COUNT, 0x10FFFFu, &fallbackIndex) == XUD_OK, "lookup-miss-query",
        "a codepoint the set does not cover is not a refusal");
    xud_check(rows[fallbackIndex].codepoint == XUD_FONT_FALLBACK_CODEPOINT, "lookup-fallback",
        "and answers the row that holds the question mark, which is raylib's GetGlyphIndex fallback rather than an error per character");
    xud_check(xud_font_glyph_index(rows, XUD_TEST_GLYPH_COUNT, 65u, NULL) == XUD_INVALID_ARG, "lookup-null-out",
        "an index with nowhere to be written is refused");
    xud_check(xud_font_glyph_index(rows, 0u, 65u, &index) == XUD_INVALID_ARG, "lookup-empty-set",
        "and a set with no rows has no index to answer");

    xud_check(xud_font_glyph_info(&info, rows, XUD_TEST_GLYPH_COUNT, letterIndex) == XUD_OK, "info-query",
        "the metrics of that row are readable");
    xud_check(info.value == 65u, "info-value", "and the row is the codepoint in hand");
    xud_check(info.advanceX > 0.0f, "info-advance",
        "and a letter of a real font has a positive advance, which is what makes the measurement below a sum rather than a zero");
    xud_check(xud_font_glyph_info(&info, rows, XUD_TEST_GLYPH_COUNT, XUD_TEST_GLYPH_COUNT) == XUD_ALREADY, "info-past-end",
        "an index at or past the rows is the caller holding on rather than a question about the font");

    xud_check(xud_font_glyph_atlas_rec(&rec, rows, XUD_TEST_GLYPH_COUNT, letterIndex) == XUD_OK, "rec-query",
        "and the same row's box is readable beside it");
    xud_check(rec.width > 0.0f && rec.height > 0.0f, "rec-inked", "a letter has an outline, so its box is not empty");
    xud_check((rec.x + rec.width) <= (float)XUD_TEST_ATLAS_DIM, "rec-inside-x",
        "and the box is inside the atlas's width, which is the one fact the packer guarantees about a box and the only place a box could "
        "go wrong");
    xud_check((rec.y + rec.height) <= (float)XUD_TEST_ATLAS_DIM, "rec-inside-y", "and the same along the other axis");

    xud_check(xud_font_glyph_atlas_rec(&rec, rows, XUD_TEST_GLYPH_COUNT, 0u) == XUD_OK, "space-rec-query",
        "the set's first row is the space and its box is readable");
    xud_check(rows[0u].codepoint == XUD_FONT_MIN_CODEPOINT, "space-is-first", "and that row is the space");
    xud_check(rec.width == 0.0f, "space-empty-box",
        "and its box is empty: a space has no outline, and the answer is an empty box rather than an absent row - the advance still moves "
        "the pen");

    for (i = 0u; i < (XUD_TEST_ATLAS_DIM * XUD_TEST_ATLAS_DIM); i++) {
        if (coverage[i] != 0u) {
            ink++;
        }
    }
    xud_check(ink > 0u, "coverage-has-ink",
        "a real font at this height writes coverage somewhere, which is what makes the conversion below a conversion rather than a copy of "
        "nothing");

    xud_check(xud_font_atlas_rgba(&rgba, coverage, XUD_TEST_ATLAS_DIM) == XUD_OK, "convert-query",
        "the coverage is convertible to the one texel format the texture rung has");
    if (rgba == NULL) {
        free(coverage);
        return;
    }
    xud_check(rgba[0u] == XUD_FONT_INK_CHANNEL && rgba[1u] == XUD_FONT_INK_CHANNEL && rgba[2u] == XUD_FONT_INK_CHANNEL,
        "convert-ink-channel", "the three colour channels are the constant this rung states, so a draw's tint is what colours a glyph");
    xud_check(rgba[3u] == coverage[0u], "convert-alpha-is-coverage",
        "and the packer's one channel becomes the texel's alpha rather than a threshold, which is what keeps a glyph's edge a gradient");
    xud_check(
        rgba[((XUD_TEST_ATLAS_DIM * XUD_TEST_ATLAS_DIM) - 1u) * 4u + 3u] == coverage[((XUD_TEST_ATLAS_DIM * XUD_TEST_ATLAS_DIM) - 1u)],
        "convert-last-texel", "and the same at the far corner, so the conversion is one pass rather than a prefix");

    xud_check(
        xud_font_measure(&width, rows, XUD_TEST_GLYPH_COUNT, XUD_TEST_FONT_SIZE, oneChar, 1u, (float)XUD_TEST_FONT_SIZE, 0.0f) == XUD_OK,
        "measure-one-query", "one glyph at the bake's own height is measurable");
    xud_check(width == rows[letterIndex].advanceX, "measure-one-value",
        "and the answer is that glyph's advance exactly - the scale is one and the spacing is zero, so the measurement is the advance and "
        "nothing else");
    xud_check(xud_font_measure(&width, rows, XUD_TEST_GLYPH_COUNT, XUD_TEST_FONT_SIZE, pair, 2u, (float)XUD_TEST_FONT_SIZE, 0.0f) == XUD_OK,
        "measure-pair-query", "two glyphs are measurable");
    xud_check(width == (rows[letterIndex].advanceX + rows[letterIndex + 1u].advanceX), "measure-pair-value",
        "and the answer is the two advances summed, which is the pen's position after the last glyph rather than the ink's width");
    xud_check(xud_font_measure(&width, rows, XUD_TEST_GLYPH_COUNT, XUD_TEST_FONT_SIZE, pair, 2u, (float)(XUD_TEST_FONT_SIZE * 2u), 0.0f)
            == XUD_OK,
        "measure-scaled-query", "the same pair at twice the height is measurable without a second bake");
    xud_check(width == (2.0f * (rows[letterIndex].advanceX + rows[letterIndex + 1u].advanceX)), "measure-scaled-value",
        "and the answer is the same sum scaled, which is the reason a font carries a base size at all - a second bake would be a second "
        "atlas per size");
    xud_check(xud_font_measure(&width, rows, XUD_TEST_GLYPH_COUNT, XUD_TEST_FONT_SIZE, pair, 2u, (float)XUD_TEST_FONT_SIZE, 1.0f) == XUD_OK,
        "measure-spacing-query", "the same pair with a spacing after each glyph is measurable");
    xud_check(width == ((rows[letterIndex].advanceX + 1.0f) + (rows[letterIndex + 1u].advanceX + 1.0f)), "measure-spacing-value",
        "and the spacing is counted after the last glyph as well as after the first, which is raylib's measure rather than an ink width");
    xud_check(xud_font_measure(&width, rows, XUD_TEST_GLYPH_COUNT, XUD_TEST_FONT_SIZE, pair, 2u, (float)XUD_TEST_FONT_SIZE, 0.0f) == XUD_OK,
        "measure-fallback-query", "a codepoint the set does not cover is measured as the fallback row");
    xud_check(xud_font_measure(NULL, rows, XUD_TEST_GLYPH_COUNT, XUD_TEST_FONT_SIZE, oneChar, 1u, 1.0f, 0.0f) == XUD_INVALID_ARG,
        "measure-null-out", "a width with nowhere to be written is refused");
    xud_check(xud_font_measure(&width, rows, XUD_TEST_GLYPH_COUNT, XUD_TEST_FONT_SIZE, NULL, 1u, 1.0f, 0.0f) == XUD_INVALID_ARG,
        "measure-no-text", "and a string with no characters behind it is refused rather than answered as zero");
    xud_check(xud_font_measure(&width, rows, XUD_TEST_GLYPH_COUNT, XUD_TEST_FONT_SIZE, oneChar, 0u, 1.0f, 0.0f) == XUD_INVALID_ARG,
        "measure-empty-text", "and so is a string whose count is zero, which is a question a caller has to mean something else by");
    xud_check(xud_font_measure(&width, rows, XUD_TEST_GLYPH_COUNT, 0u, oneChar, 1u, 1.0f, 0.0f) == XUD_UNSUPPORTED, "measure-no-base",
        "and a record that never had a base height has nothing to scale against, which is this rung's refusal rather than a division by "
        "zero");

    free(coverage);
    free(rgba);
}

static void xud_test_font_bake_refused(void) {
    XudFontData data;
    XudGlyphRow rows[4];
    uint8_t* coverage = NULL;
    const uint32_t set[1] = { 65u };

    memset(rows, 0, sizeof(rows));
    memset(&data, 0, sizeof(data));
    data.fileData = (const uint8_t*)"not a font at all, not even close";
    data.fileBytes = 35u;
    data.pixelSize = XUD_TEST_FONT_SIZE;
    data.codepoints = set;
    data.codepointCount = 1u;

    coverage = NULL;
    xud_check(xud_font_bake(&data, XUD_FONT_ATLAS_MIN_DIM, rows, &coverage) == XUD_UNSUPPORTED, "bake-not-a-font",
        "a buffer whose sfnt header stb cannot find is refused rather than baked as an empty atlas");
    xud_check(coverage == NULL, "bake-not-a-font-clears", "and no atlas is handed back, so a refusal is not a half-atlas to free");

    data.fileData = xudTestFontFile;
    data.fileBytes = xudTestFontBytes;
    coverage = NULL;
    xud_check(xud_font_bake(&data, XUD_FONT_ATLAS_MIN_DIM - 1u, rows, &coverage) == XUD_INVALID_ARG, "bake-dim-below",
        "an extent below this rung's floor is refused rather than doubled into a retry the bake cannot do");
    xud_check(xud_font_bake(&data, XUD_FONT_ATLAS_MAX_DIM * 2u, rows, &coverage) == XUD_INVALID_ARG, "bake-dim-above",
        "and so is one above its ceiling, which is the extent the texture rung below would refuse anyway");
    xud_check(xud_font_bake(&data, XUD_FONT_ATLAS_MIN_DIM, rows, NULL) == XUD_INVALID_ARG, "bake-null-out",
        "and an atlas with nowhere to be written is refused");
    xud_check(xud_font_bake(NULL, XUD_FONT_ATLAS_MIN_DIM, rows, &coverage) == XUD_INVALID_ARG, "bake-null-description",
        "and a bake with no description has nothing to bake");
    xud_check(coverage == NULL, "bake-refusals-clear", "and none of the refusals above left an atlas behind");
}

/*-----------------------------------------------------------------------------------------------
 * The public pair and the four public queries, against a real context
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_font_gpu(void) {
    XudContext* context = NULL;
    XudFontData data;
    XudHandle handle;
    XudHandle stale;
    XudGlyphInfo info;
    XudGlyphAtlasRec rec;
    const XudFontRecord* record = NULL;
    uint32_t live = 0u;
    uint32_t closed = 0u;
    float width = 0.0f;
    const uint32_t oneChar[1] = { 65u };

    memset(&handle, 0, sizeof(handle));
    memset(&data, 0, sizeof(data));
    data.fileData = xudTestFontFile;
    data.fileBytes = xudTestFontBytes;
    data.pixelSize = XUD_TEST_FONT_SIZE;
    data.codepoints = NULL;
    data.codepointCount = 0u;

    xud_check(xud_init_context(&context, 64u, 64u) == XUD_OK, "gpu-context",
        "a context comes up for the font half, so the atlas has a device to be created on");
    if (context == NULL) {
        return;
    }

    xud_check(xud_load_font(&handle, context, &data) == XUD_OK, "gpu-load",
        "the public load bakes the default set and hands the atlas down to the texture rung");
    xud_check(handle.kind == XUD_HANDLE_KIND_FONT, "gpu-load-kind",
        "and the handle it hands out is tagged FONT, which is what makes a font handle refused by the texture rung's close");
    xud_check(xud_load_font(&stale, context, NULL) == XUD_INVALID_ARG, "gpu-load-null-description",
        "a load with no description is refused before a slot is spent");
    xud_check(xud_load_font(NULL, context, &data) == XUD_INVALID_ARG, "gpu-load-null-out",
        "and a handle with nowhere to be written is refused the same way");
    if (handle.kind != XUD_HANDLE_KIND_FONT) {
        xud_close_context(&context);
        return;
    }

    xud_check(xud_font_get(context->handleTable, handle, &record) == XUD_OK, "gpu-get",
        "the record behind the slot is readable, which is what the four public queries below reach through");
    if (record == NULL) {
        xud_close_context(&context);
        return;
    }
    xud_check(record->pixelSize == XUD_TEST_FONT_SIZE, "gpu-record-size",
        "the record carries the bake's own height, which is the divisor every measurement below divides by");
    xud_check(record->glyphCount == XUD_TEST_GLYPH_COUNT, "gpu-record-count",
        "and the set's length, which is the count the lookup below searches over");
    xud_check(record->padding == XUD_FONT_ATLAS_PADDING, "gpu-record-padding",
        "and the packer's padding, which is the clear space that keeps one glyph's fetch from bleeding into the next");
    xud_check(record->atlas.kind == XUD_HANDLE_KIND_TEXTURE, "gpu-atlas-kind",
        "the atlas is a TEXTURE slot this rung owns rather than a driver object of its own, which is the second rung that owns other "
        "handles' shape");
    xud_check(record->atlasWidth == record->atlasHeight, "gpu-atlas-square",
        "and the atlas is square, so the extent is one number rather than two that can disagree");
    xud_check(record->atlasWidth >= XUD_FONT_ATLAS_MIN_DIM, "gpu-atlas-attempt",
        "and it is at least this rung's floor, so the attempt loop below settled on a real extent rather than on zero");

    xud_check(xud_get_glyph_info(&info, context, handle, 65u) == XUD_OK, "gpu-info-query",
        "the public metrics query answers for a codepoint rather than for an index, which is raylib's shape");
    xud_check(info.value == 65u, "gpu-info-value", "and the row it found is the codepoint in hand");
    xud_check(xud_get_glyph_atlas_rec(&rec, context, handle, 65u) == XUD_OK, "gpu-rec-query",
        "and the same codepoint's box is readable beside it");
    xud_check(rec.width > 0.0f && rec.height > 0.0f, "gpu-rec-inked",
        "a letter's box is not empty, so the atlas the texture rung made has this glyph in it");
    xud_check((rec.x + rec.width) <= (float)record->atlasWidth, "gpu-rec-inside",
        "and the box is inside the extent the record says, which is the one fact that ties the rows to the uploaded atlas");

    xud_check(xud_measure_text(&width, context, handle, oneChar, 1u, (float)XUD_TEST_FONT_SIZE, 0.0f) == XUD_OK, "gpu-measure-query",
        "the public measurement answers");
    xud_check(width == info.advanceX, "gpu-measure-value",
        "and answers that glyph's advance at the bake's own height, which is the same number the pure half's measurement answered - the "
        "two halves are one bake rather than two hopes");

    xud_check(xud_measure_text(NULL, context, handle, oneChar, 1u, 1.0f, 0.0f) == XUD_INVALID_ARG, "gpu-measure-null-out",
        "a width with nowhere to be written is refused");
    xud_check(xud_measure_text(&width, context, handle, oneChar, 0u, 1.0f, 0.0f) == XUD_INVALID_ARG, "gpu-measure-empty",
        "and so is an empty string");
    xud_check(xud_get_glyph_info(&info, context, handle, 0u) == XUD_OK, "gpu-info-fallback-query",
        "a codepoint the set does not cover still answers");
    xud_check(info.value == XUD_FONT_FALLBACK_CODEPOINT, "gpu-info-fallback",
        "and answers the question mark's row rather than a refusal, so a measure and a draw of one string cannot disagree");

    xud_check(xud_close_font(context, handle) == XUD_OK, "gpu-close",
        "the public close closes the atlas through the texture rung below and frees the rows above it");
    xud_check(xud_font_get(context->handleTable, handle, &record) == XUD_INVALID_ARG, "gpu-get-after-close",
        "the handle no longer names a live slot");
    xud_check(xud_handle_table_live_count(context->handleTable, XUD_HANDLE_KIND_FONT, &live) == XUD_OK, "gpu-font-live-query",
        "the FONT live count answers after the close");
    xud_check(live == 0u, "gpu-font-live", "and the font's own slot is gone");
    xud_check(xud_handle_table_live_count(context->handleTable, XUD_HANDLE_KIND_TEXTURE, &live) == XUD_OK, "gpu-texture-live-query",
        "and the TEXTURE live count answers too, which is the atlas having been closed rather than left behind");
    xud_check(live == 0u, "gpu-texture-live",
        "and answers zero, so the font's close ran the texture rung's close below it rather than leaking the atlas into the allocator's "
        "teardown");

    xud_check(xud_close_font(context, handle) == XUD_INVALID_ARG, "gpu-close-twice",
        "a close run twice is refused by the generation bump rather than double-freeing the rows");

    stale = handle;
    stale.generation = handle.generation + 1u;
    xud_check(xud_close_font(context, stale) == XUD_INVALID_ARG, "gpu-close-stale",
        "and a handle kept past the release that freed its slot is refused before anything is freed");
    xud_check(xud_get_glyph_info(&info, context, stale, 65u) == XUD_INVALID_ARG, "gpu-info-stale",
        "and so for the queries, which is the same validation the close runs");
    xud_check(xud_measure_text(&width, context, stale, oneChar, 1u, 1.0f, 0.0f) == XUD_INVALID_ARG, "gpu-measure-stale",
        "and so for the measurement");

    xud_check(xud_font_close_all(context->handleTable, context->loader, context->device, context->allocator, &closed) == XUD_OK,
        "gpu-close-all-idempotent", "the teardown rung runs again with nothing left and is not refused");
    xud_check(closed == 0u, "gpu-close-all-count",
        "and it closed nothing, so the rung above the texture walk is idempotent rather than a double free waiting for it");

    xud_close_context(&context);
    xud_check(context == NULL, "gpu-context-closed", "the context is closed and cleared, so the half leaves no atlas behind");
}

void xud_test_font(void) {
    if (xud_test_font_file_read() == 0) {
        printf("note: no font file at " XUD_TEST_FONT_PATH ", so the bake and the GPU half are skipped\n");
        xud_test_font_check();
        xud_test_font_default();
        xud_test_font_dim();
        xud_test_font_stb();
        return;
    }
    xud_test_font_check();
    xud_test_font_default();
    xud_test_font_dim();
    xud_test_font_stb();
    xud_test_font_bake_pure();
    xud_test_font_bake_refused();
    if (xud_gpu_tests_enabled()) {
        xud_test_font_gpu();
    }
    xud_test_font_file_drop();
}
