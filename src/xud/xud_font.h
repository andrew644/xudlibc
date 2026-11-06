/***********************************************************************************************
 *
 *   xud_font.h - the font rung: stb_truetype's bake above one host atlas, the glyph rows that
 *                atlas was packed from, and the load/close pair xud.h declares on top of a
 *                TEXTURE slot this rung owns
 *
 *   Owns : XudFontRecord - the record behind a FONT-kind slot of the handle layer - plus the
 *          pure halves above it (xud_font_check(), xud_font_default_count(), xud_font_atlas_dim
 *          (), xud_font_bake(), xud_font_atlas_rgba(), xud_font_glyph_index(), xud_font_glyph_at
 *          las_rec(), xud_font_glyph_info(), xud_font_measure(), xud_font_result_from_stb()) and
 *          the pen walk above the shapes rung (xud_font_append_text()), the accessor (xud_font_g
 *          et()), the driver halves (xud_font_open(), xud_font_close()) and the teardown rung abo
 *          ve the table's own close (xud_font_close_all()).
 *   Layer: above the texture rung and below xud.h. This is the SECOND rung that owns other hand
 *          les (the model's owns MESH and MATERIAL slots, this one owns exactly one TEXTURE slot
 *          : the glyph atlas) and the only place in this tree that calls stb_truetype at all. Th
 *          e atlas is a texture and so is uploaded by the texture rung rather than by a second u
 *          pload path here, which is why this rung calls no Vulkan entry point of its own any m
 *          ore than the model rung calls none.
 *
 *   Why the bake is host-only and the upload is the texture rung's: stb_truetype's packer writes
 *   one channel of coverage into a host buffer, and the texture rung above already owns the whole
 *   path from host bytes to a sampled image (the staging arena, the per-mip copy, the layout trans
 *   ition, the sampler). A second upload path in this rung would be a second copy of that path sta
 *   ling on the one the texture rung's own notes already settle, so the font rung's driver half is
 *   one xud_texture_open() call and nothing else. What the font rung adds on top of that is the on
 *   e thing the texture rung cannot know: which texels are which glyphs, and how far the pen moves
 *   after each one.
 *
 *   Why the codepoint array is sorted and unique rather than free-form: the lookup every draw and
 *   every measure owes a glyph is a binary search over the rows, and a binary search needs the arra
 *   y ordered. raylib's LoadFontEx takes an unsorted array and then sorts it internally (text.c: th
 *   e codepoints are sorted before the atlas is packed), so the requirement is raylib's own, stated
 *   here as a precondition on the description rather than as a sort this rung performs behind a cal
 *   ler's back - a sort would move a caller's rows relative to the array it handed, and a caller th
 *   at kept its own index into that array would then be reading the wrong glyph.
 *
 *   What this rung owns of the sampler's half, and why: the entry of set 1's sampler array the atlas
 *   occupies. The open calls for it (xud_descriptor_bind_texture_slot, the same pair the model rung
 *   uses for a map) and the close gives it back before it destroys the view and sampler the entry
 *   names - an entry naming a destroyed view is undefined behaviour, and the release is what keeps
 *   the slot's rule from being broken at the close. The walk draws through the textured column of
 *   the pipeline list (shaders/solid_textured.frag and set 1's sampler array), and the slot it draws
 *   with is the atlas's own entry of that array, resolved through the texture record the atlas is -
 *   the walk takes the slot as an argument and the public pair hands it down, so a record whose atlas
 *   occupies no entry of the array draws through slot 0's white default rather than through a refusal. Also absent for the reason the model rung states for its own omissions:
 *   no SDF generation (raylib's FONT_SDF is a second atlas format and a second variant reading it),
 *   no bitmap-font (.fnt/.bmf) reader and no sprite-font reader, all three of which are file formats
 *   stb_truetype does not read at all.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#ifndef XUD_XUD_FONT_H_
#define XUD_XUD_FONT_H_

#include "stb_truetype.h" /* Required for: stbtt_pack_context, stbtt_packedchar, stbtt_pack_range, stbtt_fontinfo - the bake this rung runs. Included again here on purpose: the implementation lives in xud_vendor_stb_truetype.c, and this header is the caller's half of that split, the same way xud_model.h includes cgltf.h. */
#include "xud.h" /* Required for: XudResult, XudHandle, XudFontData, XudGlyphInfo, XudGlyphAtlasRec, XUD_HANDLE_KIND_FONT - the public surface this rung is the half of */
#include "xud_allocator.h" /* Required for: XudAllocator, handed down to the texture rung's block */
#include "xud_batch.h" /* Required for: XudBatch - the batch a glyph quad is appended into, which is the batch the renderer owns */
#include "xud_device.h" /* Required for: XudDevice, the VkDevice the atlas this rung uploads belongs to */
#include "xud_handle.h" /* Required for: XudHandleTable, xud_handle_validate/alloc/free, xud_handle_record_get/set - the slot this record lives in, and the TEXTURE slot it owns */
#include "xud_loader.h" /* Required for: XudLoader, handed down to the texture rung */
#include "xud_physical.h" /* Required for: XudDeviceCaps, handed down to the texture rung's caps gate */
#include "xud_texture.h" /* Required for: XUD_TEXTURE_FILTER_*, XUD_TEXTURE_ADDRESS_*, XUD_TEXTURE_MAX_TEXELS, XUD_TEXTURE_BYTES_PER_TEXEL, xud_texture_open/close - the atlas this rung uploads */

#include <assert.h> /* Required for: static_assert, the atlas bound against the texture rung's own */
#include <stdint.h> /* Required for: uint32_t - the codepoint type and every count below */
#include <stdlib.h> /* Required for: calloc, free - the record, the rows and the atlas */

/*-----------------------------------------------------------------------------------------------
 * The rung's own bounds
 *-----------------------------------------------------------------------------------------------
 *
 * Every number below is this rung's ceiling rather than a driver fact, and each is stated against
 *   the rung below's own bound rather than against an invented number: the atlas IS a texture, so i
 *   ts extent is legal only if the texture rung's check accepts it, and the glyph rows ARE an alloc
 *   ation, so their count is bounded by the same arithmetic the texture rung's texel bound uses.
 *
 * The atlas is square and its side is a power of two, doubling from XUD_FONT_ATLAS_MIN_DIM until th
 *   e packer accepts. The starting side is the rung's own rather than the texture's: a 64x64 atlas i
 *   s 16 KiB of host bytes and the doubling sequence reaches the ceiling in four steps, so a bake th
 *   at does not fit costs three retries rather than one oversized atlas.
 */

#define XUD_FONT_ATLAS_MIN_DIM 64u /* the smallest atlas side this rung will try */
#define XUD_FONT_ATLAS_MAX_DIM 1024u /* the largest atlas side this rung will try */
#define XUD_FONT_ATLAS_PADDING 1u /* texels of clear space between packed glyphs - raylib's Font.glyphPadding */
#define XUD_FONT_MIN_PIXEL_SIZE 4u /* the smallest bake this rung accepts, in pixels of em height */
#define XUD_FONT_MAX_PIXEL_SIZE 96u /* the largest bake this rung accepts, in pixels of em height */
#define XUD_FONT_MAX_GLYPHS 512u /* glyphs per font: this rung's ceiling, against the atlas's box budget below */
#define XUD_FONT_DEFAULT_FIRST 32u /* the default set's first codepoint - raylib's default charset starts at the space */
#define XUD_FONT_DEFAULT_LAST 126u /* the default set's last codepoint - raylib's default charset ends at the tilde */
#define XUD_FONT_DEFAULT_COUNT 95u /* the default set's length - raylib's 32..126, the printable ASCII run */
#define XUD_FONT_MIN_FILE_BYTES 12u /* the shortest buffer this rung looks at: sfnt's header plus one table */
#define XUD_FONT_FALLBACK_CODEPOINT 63u /* '?' - the glyph a codepoint outside the set answers, raylib's GetGlyphIndex fallback */
#define XUD_FONT_INK_CHANNEL 255u /* a glyph texel's colour channels: the packer's one channel IS the coverage */
#define XUD_FONT_MIN_CODEPOINT 32u /* the lowest codepoint this rung bakes: below it there is no glyph to see */
#define XUD_FONT_MAX_CODEPOINT 0x10FFFFu /* the highest codepoint this rung bakes: Unicode's own top */

static_assert((XUD_FONT_ATLAS_MIN_DIM * XUD_FONT_ATLAS_MIN_DIM) >= XUD_FONT_ATLAS_MIN_DIM,
    "xud_font.h: the smallest atlas is a real square, so the products below are products and not overflows");
static_assert(((XUD_FONT_ATLAS_MAX_DIM * XUD_FONT_ATLAS_MAX_DIM) * XUD_TEXTURE_BYTES_PER_TEXEL)
        <= (XUD_TEXTURE_MAX_TEXELS * XUD_TEXTURE_BYTES_PER_TEXEL),
    "xud_font.h: the largest atlas this rung will try is inside the texture rung's own texel bound, so a bake that reaches the ceiling is "
    "a refusal and never a texture the rung below would refuse mid-upload");
static_assert((XUD_FONT_ATLAS_MAX_DIM % XUD_FONT_ATLAS_MIN_DIM) == 0u,
    "xud_font.h: the doubling sequence from the floor reaches the ceiling exactly, so the loop below has no off-by-one at its top");
static_assert(XUD_FONT_ATLAS_PADDING < XUD_FONT_MIN_PIXEL_SIZE,
    "xud_font.h: the padding is smaller than the smallest glyph, so a glyph box is never mostly padding");
static_assert(XUD_FONT_DEFAULT_COUNT > 0u, "xud_font.h: the default set is never empty");
static_assert(
    XUD_FONT_DEFAULT_FIRST >= XUD_FONT_MIN_CODEPOINT, "xud_font.h: the default set starts at or above this rung's lowest codepoint");
static_assert((XUD_FONT_DEFAULT_FIRST + XUD_FONT_DEFAULT_COUNT) <= (XUD_FONT_MAX_CODEPOINT + 1u),
    "xud_font.h: the default set ends inside the range this rung admits");
static_assert(
    XUD_FONT_INK_CHANNEL <= 255u, "xud_font.h: the ink channel is a byte, so it survives the texel format's one-byte-per-channel below");
static_assert((XUD_FONT_FALLBACK_CODEPOINT >= XUD_FONT_MIN_CODEPOINT) && (XUD_FONT_FALLBACK_CODEPOINT <= XUD_FONT_MAX_CODEPOINT),
    "xud_font.h: the fallback is inside the range this rung admits, so a default set can hold it");
static_assert(XUD_FONT_MIN_CODEPOINT <= XUD_FONT_MAX_CODEPOINT, "xud_font.h: the admitted range is not empty");
static_assert(XUD_FONT_MIN_PIXEL_SIZE <= XUD_FONT_MAX_PIXEL_SIZE, "xud_font.h: the size range is not empty");
static_assert(XUD_FONT_MAX_GLYPHS <= (XUD_FONT_ATLAS_MAX_DIM * XUD_FONT_ATLAS_MAX_DIM),
    "xud_font.h: a glyph needs at least one texel, so the row bound cannot exceed the atlas's texel count");
static_assert(XUD_FONT_MAX_CODEPOINT == 0x10FFFFu,
    "xud_font.h: the admitted range tops out at Unicode's own top, which is what makes every codepoint below it fit the int stb's own API "
    "names");

/* The area a glyph box costs, in texels: the bake's pixel height plus the padding on both sides,
   squared. The atlas bound below is stated in boxes rather than in texels because the packer's own
   guarantee is one box per glyph, and the box is what grows with the size. */
#define XUD_FONT_GLYPH_BOX(pixelSize) (((pixelSize) + (2u * XUD_FONT_ATLAS_PADDING)) * ((pixelSize) + (2u * XUD_FONT_ATLAS_PADDING)))

/* How many boxes of one size an atlas of one side can hold: the atlas's texel count divided by the
   box's area. A BUDGET rather than a fit - the packer's packing is not perfect, so a set that fits
   this budget can still be refused, and the doubling loop below is what turns that refusal into a
   bigger attempt. The check and the dim rung ask the same question at two points, which is why this
   is a macro rather than a function the two of them would call.
*/
#define XUD_FONT_ATLAS_BUDGET(dim, pixelSize) (((dim) * (dim)) / XUD_FONT_GLYPH_BOX(pixelSize))

static_assert(XUD_FONT_DEFAULT_LAST >= XUD_FONT_DEFAULT_FIRST, "xud_font.h: the default set's bounds are a span and not a wrap");
static_assert((XUD_FONT_DEFAULT_LAST - XUD_FONT_DEFAULT_FIRST + 1u) == XUD_FONT_DEFAULT_COUNT,
    "xud_font.h: the default set's count is its bounds' span, so a test that pins the bounds pins the count");
static_assert(XUD_FONT_MIN_FILE_BYTES >= 4u,
    "xud_font.h: the shortest buffer is at least sfnt's own version word, so the sniff below reads inside it");

/*-----------------------------------------------------------------------------------------------
 * One glyph's row: what the packer answered, and what a draw and a measure both read
 *
 * The row is the packer's stbtt_packedchar read into this library's own vocabulary: the atlas box
 *   stays integers (it names texels), and the three pen numbers stay floats (they are subpixel offse
 *   ts and a fractional advance, which is the whole point of stb's subpixel positioning). raylib's G
 *   lyphInfo carries the same three plus the codepoint, and its atlas position is a separate Rectang
 *   le array; keeping the two halves in one row here is what makes a lookup one index rather than two
 *   parallel arrays that can disagree.
 */

typedef struct XudGlyphRow {
    uint32_t codepoint; /* the codepoint this row was baked for - the search key, and the value a caller reads back */
    uint32_t atlasX; /* the box's left texel in the atlas */
    uint32_t atlasY; /* the box's top texel in the atlas */
    uint32_t atlasWidth; /* the box's width in texels, the packer's own answer and never the em box */
    uint32_t atlasHeight; /* the box's height in texels */
    float offsetX; /* the pen's horizontal step to the box's left edge at the baseline - raylib's GlyphInfo.offsetX */
    float offsetY; /* the pen's vertical step to the box's top edge - raylib's GlyphInfo.offsetY */
    float advanceX; /* how far the pen moves after this glyph, in bake pixels - raylib's GlyphInfo.advanceX */
} XudGlyphRow;

static_assert(sizeof(XudGlyphRow) == (4u * 8u),
    "xud_font.h: a row is four counts and three floats plus the codepoint, so the rows array is one allocation of a known stride");

/*-----------------------------------------------------------------------------------------------
 * What a FONT-kind slot records
 *
 *   The record owns the rows array and nothing of the driver's: the atlas's bytes are already inside
 *   the TEXTURE slot this rung opened, and the host copy of them is transient - allocated inside the
 *   open, freed before it returns, which is the same rule the texture rung's own note states for a
 *   record that reached the write ("the bytes the upload read are already inside the image"). The te
 *   xture handle is stored by value rather than looked up from a name, so a font's atlas cannot be
 *   closed twice through two paths and cannot be left dangling by a caller that closed it first: the
 *   font's own close closes it, and a caller that closes the atlas behind the font's back is a mista
 *   ke the handle layer's own kind check turns into a refusal.
 *
 *   The atlas extent is a member rather than recomputed because the doubling loop above chose it and
 *   nothing below can recover which attempt succeeded.
 *-----------------------------------------------------------------------------------------------*/

typedef struct XudFontRecord {
    uint32_t pixelSize; /* the bake's em height, in pixels - raylib's Font.baseSize */
    uint32_t padding; /* the packer's inter-glyph padding - raylib's Font.glyphPadding */
    uint32_t glyphCount; /* rows below, the description's count read back */
    uint32_t atlasWidth; /* the atlas's side: the square this rung's dim loop settled on */
    uint32_t atlasHeight; /* the same number, kept as a pair so the record reads as an extent rather than as a dim */
    XudHandle atlas; /* the TEXTURE slot this font owns - the only driver object of this rung's */
    XudGlyphRow* glyphs; /* the rows the packer filled, one host allocation owned by this record */
} XudFontRecord;

static_assert(XUD_HANDLE_KIND_FONT == 2u, "xud_font.h: the FONT kind is the tag the handle layer's own table reserves for this rung");

/*-----------------------------------------------------------------------------------------------
 * The layer's contract
 *-----------------------------------------------------------------------------------------------*/

/// Pure: the argument check every entry point below runs before a single byte is allocated. NULL des
/// cription; a NULL fileData or a fileBytes of zero (a font with no bytes is not a font with a defau
/// lt); a fileBytes below the smallest sfnt header stb's own sniff reads (a buffer that short cannot
/// be told from a font at all, so the refusal is this rung's rather than the packer's); a pixelSize o
/// utside this rung's size range (the bake's em height is what the box arithmetic above multiplies, a
/// nd a size outside the range is a description this rung cannot lay out); a codepointCount past XUD_
/// FONT_MAX_GLYPHS (no atlas here can hold that many boxes, see the bound's own note); a NULL codepoi
/// nts array paired with a non-zero count (unlike a mesh's optional streams, an array named by a coun
/// t has to be there); a codepoint outside the admitted range (below the lowest there is no glyph to see,
///  and above Unicode's own top stb's int codepoints cannot name one); a codepoint array that is not strictly ascending (the lookup below
///  is a
/// binary search, and the reason the precondition is stated here rather than as a sort this rung runs
/// is the note above); and the box budget - glyphCount boxes of the size above must fit the largest at
/// this this rung will try, which is the refusal stated before the first allocation rather than after
/// the last retry. When the description asks for the default set (a NULL array with a zero count) *ou
/// tGlyphCount, when not NULL, answers XUD_FONT_DEFAULT_COUNT; otherwise it answers the description's
/// own count. XUD_INVALID_ARG on any of these or on a NULL out-count that is asked for; XUD_OK otherw
/// ise.
XudResult xud_font_check(const XudFontData* data, uint32_t* outGlyphCount);

/// Pure: the default set's own count, which is the one number the check above answers for a descript
/// ion that asked for the default rather than naming codepoints. Its own rung rather than a constant
/// read at the call sites because it is the pair (first, count) that the default set's rows are stated
/// against in the tests below, and a test that pinned the count without the first would pin half of a
/// set. *outFirst and *outCount answer the set's bounds. XUD_INVALID_ARG on a NULL out-param either half
/// is asked for; XUD_OK otherwise.
XudResult xud_font_default_count(uint32_t* outFirst, uint32_t* outCount);

/// Pure: the atlas side a bake should start at - the smallest power of two at or above XUD_FONT_ATLAS
/// _MIN_DIM whose square holds the description's glyph boxes, clamped to this rung's own range. The st
/// art is a guess rather than the packer's answer (stb's own packing is "very crappy", per its note abo
/// ve the bake call, and no bound on this side of it is tight), which is why the driver half doubles it
/// and retries rather than trusting it: this number is the first attempt, and the only thing it buys i
/// s that the common case costs one attempt rather than four. XUD_INVALID_ARG on a NULL out-dim, on a c
/// ount past this rung's bound or on a size outside the range; XUD_UNSUPPORTED when even the largest at
/// this rung will try cannot hold the boxes - which is the same refusal the check above states before an
/// ything is spent, and is kept here as well so the dim rung cannot be asked to lie about a set that do
/// es not fit; XUD_OK otherwise.
XudResult xud_font_atlas_dim(uint32_t glyphCount, uint32_t pixelSize, uint32_t* outDim);

/// Pure: the bake itself - stb_truetype's own three steps (the font header read, the pack context open
/// ed over the atlas buffer, one pack range naming the description's codepoint array) run in that order
/// over a host atlas this rung allocates and hands back. The buffer is one channel of coverage because
/// that is what the packer writes; the RGBA form the texture rung wants is the conversion rung below, an
/// d keeping the two apart is what lets the headless tests state the coverage arithmetic without a tex
/// ture in sight. The font index is always 0: a TrueType Collection's second font is a second font, and
/// raylib's LoadFontEx names one font per file, so the index is a constant rather than a knob. The pack
/// 's own answer is turned into this library's code by the conversion rung below rather than being retur
/// ned as an int. On XUD_OK *outCoverage names a freshly allocated dim*dim buffer the caller frees, and
/// the rows array (caller-provided, glyphCount long) is filled; on every refusal the buffer is freed and
/// *outCoverage is left NULL. XUD_INVALID_ARG on a NULL description, rows, out-coverage, a dim outside t
/// his rung's range, a count past this rung's bound or a size outside the range; XUD_UNSUPPORTED when th
/// e buffer is not a font this rung can read (stb's own header sniff refuses, which is the answer for a
/// buffer that is short, truncated or not an sfnt at all) or when the packer refuses the set at every dim
/// this rung will try; XUD_OUT_OF_MEMORY when the atlas or the pack context cannot be allocated.
XudResult xud_font_bake(const XudFontData* data, uint32_t dim, XudGlyphRow* outRows, uint8_t** outCoverage);

/// Pure: the coverage-to-texel conversion the texture rung's one format asks for. Every glyph's coverage
/// becomes a WHITE texel whose ALPHA is the coverage, which is the convention a textured draw reads with
/// out a second decision: a draw that multiplies a tint by the sampled texel gets the tint at full strengt
/// h where the glyph is and nothing where it is not, and the clear space between glyphs stays transparent
/// rather than a black box that would draw as a rectangle around every letter. The three colour channels a
/// re the same constant rather than a gradient because the packer's one channel IS the gradient. *outRgba
/// names a freshly allocated dim*dim*4 buffer the caller frees, and the coverage buffer is left alone - th
/// e two coexist for the length of the upload only. XUD_INVALID_ARG on a NULL out-rgba, a NULL coverage o
/// r a dim of zero; XUD_OUT_OF_MEMORY when the texel buffer cannot be allocated; XUD_OK otherwise.
XudResult xud_font_atlas_rgba(uint8_t** outRgba, const uint8_t* coverage, uint32_t dim);

/// Pure: the row a codepoint names, as an index into the rows array - a binary search over a sorted arr
/// ay, and the fallback raylib's GetGlyphIndex states rather than a refusal: a codepoint the set does not
/// cover answers the row that holds '?', so a caller drawing a string the font cannot render gets boxes it
/// can still measure rather than a refusal per character. When the set does not even contain '?' the fallb
/// ack is row 0, which is the one row this rung guarantees exists, and that fallback is stated here rather
/// than left to the caller because a caller cannot recover it from a refusal either. XUD_INVALID_ARG on a N
/// ULL out-index, a NULL rows or a count of zero; XUD_OK otherwise, including the two fallbacks.
XudResult xud_font_glyph_index(const XudGlyphRow* rows, uint32_t glyphCount, uint32_t codepoint, uint32_t* outIndex);

/// Pure: one row's metrics, the codepoint and the three pen numbers, without the atlas box - raylib's Ge
/// tGlyphInfo answer, and the reason the box is a separate rung below is that raylib keeps them separate
/// too (GlyphInfo and Rectangle are two arrays in its Font) and this rung's public pair mirrors that split
/// rather than inventing a third shape. XUD_INVALID_ARG on a NULL out-info or rows; XUD_ALREADY when the i
/// ndex is at or past the count - the index came from the rung above, which always answers a live one, so a
/// n out-of-range index is a caller holding on rather than a question about the font; XUD_OK otherwise.
XudResult xud_font_glyph_info(XudGlyphInfo* outInfo, const XudGlyphRow* rows, uint32_t glyphCount, uint32_t index);

/// Pure: one row's box in the atlas, as pixel coordinates rather than as texture coordinates - raylib's Ge
/// tGlyphAtlasRec answer, which is a Rectangle in texels, and the conversion to the [0,1] the sampler wants
/// is a divide by an extent this rung knows and a draw step knows, so the rung that states the box states it
/// in texels and the rung that draws converts. XUD_INVALID_ARG on a NULL out-rec or rows; XUD_ALREADY on an
/// index at or past the count, the same reason as above; XUD_OK otherwise.
XudResult xud_font_glyph_atlas_rec(XudGlyphAtlasRec* outRec, const XudGlyphRow* rows, uint32_t glyphCount, uint32_t index);

/// Pure: the width of a string in pixels at a requested em height - the sum of every glyph's advance, scaled
/// from the bake's height to the requested one, plus the spacing after each one. The scale is a division by th
/// e bake's pixelSize rather than a second bake at the requested size: raylib's MeasureTextEx does the same thi
/// ng (glyph advance times fontSize/baseSize), and a second bake would be a second atlas per size, which is th
/// e reason raylib's Font carries a baseSize at all. The trailing spacing is INCLUDED, which is raylib's own ans
/// wer (its measure returns the pen position after the last glyph, which is what a centring call wants) and is
/// stated here because it is the one place this rung's answer differs from a caller's intuition of "ink width".
/// A codepoint the set does not cover is measured as the fallback row, the same row the lookup above answers, s
/// o a measure and a draw of one string cannot disagree about where the pen ends. XUD_INVALID_ARG on a NULL out
/// -width, rows, text or a textCount of zero; XUD_UNSUPPORTED on a bake height of zero (a record that never had
/// one is not a record this rung can scale against); XUD_OK otherwise.
XudResult xud_font_measure(float* outWidth, const XudGlyphRow* rows, uint32_t glyphCount, uint32_t pixelSize, const uint32_t* text,
    uint32_t textCount, float requestedSize, float spacing);

/// Pure: stb's own answer turned into this library's code. The packer answers 1 for success and 0 for "this
/// set does not fit this atlas", and the font header's two sniff functions answer a negative offset and a zero
/// count for "this is not a font" - three shapes of refusal, two codes here: a set that does not fit is the desc
/// ription's fault and is XUD_UNSUPPORTED (the same code the texture rung gives for an extent its own bound refus
/// es), and a buffer that is not a font is XUD_UNSUPPORTED too, because this library has no way to tell a caller
/// that handed a buffer the difference between "not a font" and "a font this rung cannot read". XUD_OK on the suc
/// cess answers; XUD_UNSUPPORTED on every refusal; XUD_INTERNAL_ERROR on nothing - the default is that code so a
/// n unhandled shape still refuses rather than passing.
/*-----------------------------------------------------------------------------------------------
 * The pen walk: the string above the rows, and the one shape it appends
 *
 *   The walk is this rung's and the quad is the shapes rung's, and the split is where it is for
 *   one reason: the shapes rung has no idea what an atlas is, so it takes a uv box already divided
 *   by an extent, and this rung is where the extent is in sight, so this rung does the divide. The
 *   note beside the counts in xud_shapes.h states the same fact from the other side, and the public half
 *   of the extent xud_get_font_atlas_dim() reads out is what lets a caller do the divide itself rather
 *   than read it here.
 *
 *   The position is the PEN's start rather than the line's top-left, and that is the row's own
 *   spelling read literally: offsetY is the pen's step to the box's TOP edge, so a position that
 *   named the top-left would be a different number at every glyph. The pen then moves by the row's
 *   advanceX scaled plus the tracking after EVERY glyph including the last one, which is the same
 *   step xud_font_measure() sums, and is why a measure and a draw of one string cannot disagree
 *   about where the pen ends. The scale is the requested em height divided by the bake's own, the
 *   same division the measure does, so one atlas serves many sizes (raylib's Font.baseSize exists
 *   for that reason and this rung is the other half of it).
 *
 *   A codepoint with no outline - a space - has a box of zero width and still moves the pen, so
 *   the append is skipped and the advance is not: that is the answer the rows state rather than an
 *   absent row, and skipping both would run two words into one.
 *
 *   What the walk refuses rather than papering over: the append is per glyph rather than per
 *   string, so a string that runs out of batch capacity draws the glyphs that fit and refuses at
 *   the next glyph boundary, which is the same shape xud_draw_model() states for a model whose last
 *   mesh did not fit - a batch's rows are a list, and a partial string is a frame that drew part
 *   of it. The sampler's half the quads draw through is the note at the top of this file: the set-1
 *   slot the walk takes as an argument, the atlas's own entry of the array, and slot 0's white
 *   default when the record's atlas occupies no entry.
 *-----------------------------------------------------------------------------------------------*/

static_assert(XUD_FONT_ATLAS_MAX_DIM <= 16777216u,
    "xud_font.h: the atlas's extent is exactly representable as a float, so the divide the pen walk does is a divide by a divisor a float "
    "names exactly and a glyph's uv is not off by a rounding of the extent");

/// Pure in the shapes rung's sense: append one string's glyph quads into the batch, walking the pen
/// from the caller's position by each row's own advance plus the caller's tracking, and texturing
/// each quad with that row's own atlas box divided by the record's own extent. The walk draws its
/// quads through XUD_BATCH_VARIANT_TEXTURED with the set-1 slot textureIndex names, moving the
/// batch's material state to that row for the walk and back to the row it read it from when the
/// walk is done, so the glyphs flush as one textured draw row of their own and the shapes a caller
/// appends beside them draw with the state the walk found. A textureIndex of XUD_BATCH_NO_TEXTURE
/// is the no-map form: the row then samples slot 0 of set 1's array, the white default.
/// The walk skips the append for a codepoint whose box is empty (a space) while still moving the
/// pen, and it refuses a requested em height that is not a positive number rather than scaling by
/// zero or by a NaN into a degenerate box the shapes rung would refuse anyway. A live record with
/// no bake height, no rows or no extent is a state the open made rather than an argument a caller
/// can fix, and is XUD_INTERNAL_ERROR.
///
/// XUD_INVALID_ARG on a NULL batch, record or text array, a textCount of zero, a pixelSize that is
/// not strictly positive (which refuses zero, a negative height and a NaN in one comparison, the
/// reason the sphere's note gives for a NaN radius), or a textureIndex that is not the sentinel and
/// not an entry of set 1's sampler array; the shapes rung's own codes for each quad it appends;
/// XUD_OUT_OF_MEMORY when one glyph quad does not fit the batch, with the glyphs before it already
/// appended; XUD_INTERNAL_ERROR on a record whose own numbers are the sentinels; XUD_OK only when
/// every quad that was attempted completed.
XudResult xud_font_append_text(XudBatch* batch, const XudFontRecord* record, uint32_t textureIndex, const uint32_t* text,
    uint32_t textCount, XudVector3 position, float pixelSize, float spacing, XudColor color);

/// Pure: stb's own answer turned into this library's code. The packer answers 1 for success and 0
/// for "this set does not fit this atlas", and the font header's two sniff functions answer a
/// negative offset and a zero count for "this is not a font" - three shapes of refusal, two codes
/// here: a set that does not fit is the description's fault and is XUD_UNSUPPORTED (the same code
/// the texture rung gives for an extent its own bound refuses), and a buffer that is not a font is
/// XUD_UNSUPPORTED too, because this library has no way to tell a caller that handed a buffer the
/// difference between "not a font" and "a font this rung cannot read". XUD_OK on the success
/// answers; XUD_UNSUPPORTED on every refusal; XUD_INTERNAL_ERROR on nothing - the default is that
/// code so an unhandled shape still refuses rather than passing.
XudResult xud_font_result_from_stb(int packed, int fontOffset);

/// Pure: the accessor the rung above reaches instead of the slot array - validate against the FONT kind, then
/// hand back the record of a live slot. XUD_INVALID_ARG on a NULL out-record or table, or on a handle that does no
/// t name a live FONT slot (a stale handle, a cross-kind handle, a fabricated one), or on a live slot whose record
/// field is still the sentinel.
XudResult xud_font_get(const XudHandleTable* table, XudHandle handle, const XudFontRecord** outRecord);

/// Driver: the whole rung in one call, in the one order that is correct. Order: the loader/device guards, the argu
/// ment check (before a slot is spent, so a refusal leaves no half-font), the dim rung's first attempt, the slot, th
/// e record, then the bake loop - allocate the atlas at the attempt's dim, bake, free, double and retry until the pac
/// ker accepts or the ceiling is reached - then the coverage-to-texel conversion, then one XudTextureData stamped wit
/// h the atlas's extent, the converted texels and this rung's two sampling tokens (LINEAR rather than NEAREST, becaus
/// e a glyph's coverage is a gradient and a nearest filter turns every edge into a stair, and CLAMP rather than REPEA
/// T because an atlas is not a tileable surface and a repeat would bleed the glyph at the far edge into the glyph at t
/// his edge), then xud_texture_open() above it, then the atlas's entry of set 1's sampler array written through the
/// descriptor rung's free list (the same two steps xud_create_texture() and the model rung run for a map - a texture
/// that holds no entry of the array is one the walk's resolve answers with the no-map sentinel and the walk then
/// samples slot 0's white default rather than the atlas), then the record written into the slot through the handle
/// layer's own accessor. On any error everything created so far is retired - the entry of the array given back, the
/// atlas and the rows freed, the texture closed if it was already open, the slot's record field cleared before the
/// record is freed - and *outHandle is left naming nothing live. XUD_INVALID_ARG on a NULL out-handle, table, loader,
/// device, descriptor, caps or description, or a handle-less device; XUD_UNSUPPORTED on a closed loader, on a loader
/// whose rows this rung needs are not there, on a device that is not this loader's, on a description the check refused,
/// on a buffer that is not a font this rung can read, on a set that does not fit this rung's atlas, or on a device
/// whose maxImageDimension2D is below the atlas's extent; XUD_OUT_OF_RESOURCES at the handle layer's slot bound, when
/// the atlas's block cannot be carved, or when set 1's array holds no free entry; XUD_OUT_OF_MEMORY when the record,
/// the rows or the atlas cannot be allocated; XUD_VULKAN_ERROR or XUD_LOST when the driver refuses for its own reason.
XudResult xud_font_open(XudHandle* outHandle, XudHandleTable* table, XudLoader* loader, const XudDevice* device, const XudDeviceCaps* caps,
    XudDescriptor* descriptor, XudAllocator* alloc, const XudFontData* data);

/// Driver: release one live FONT slot and retire everything its record owns, in the reverse of the order above: the
/// wait-idle guard runs inside the texture rung's own close rather than here, because this rung owns no driver object
/// of its own and the guard that belongs to the atlas is the texture rung's. Order: the FONT handle validated, the at
/// las's entry of set 1's sampler array given back to the descriptor's free list (a refusal is a close that does not
/// run rather than an object retired over a descriptor still naming it, the public close's stated rule), the at
/// las closed through the TEXTURE rung (which runs the guard, destroys the sampler, the view and the image and release
/// s the block), the slot's record field cleared BEFORE the record is freed (the handle layer's own rule, and the reas
/// on this rung's teardown walk cannot double-free a record a caller already closed), the rows freed, the record freed,
/// the slot freed. XUD_INVALID_ARG on a NULL table, loader, device or allocator, or on a handle that does not name a l
/// ive FONT slot of this table; XUD_UNSUPPORTED on a closed loader, on a device that is not this loader's, or on the g
/// uard the texture rung runs; the device's own code when that guard cannot run.
XudResult xud_font_close(
    XudHandleTable* table, XudHandle handle, XudLoader* loader, const XudDevice* device, XudDescriptor* descriptor, XudAllocator* alloc);

/// Driver: the teardown rung above the handle table's own close - retire every record any FONT slot stills, live or r
/// eleased, and clear the field, so xud_handle_table_close() can free the arrays without leaving an atlas behind them.
/// This rung runs ABOVE xud_texture_close_all() in xud_close_context() for the reason the model rung states for its own
/// position above the mesh and the material rungs: a font record names a TEXTURE slot, so the atlas has to be closed th
/// rough this rung before the TEXTURE walk sees it, and a TEXTURE walk that ran first would leave a font record naming a
/// destroyed image. *outClosed counts the records retired, the model's and the texture's own count and reason. XUD_INVAL
/// ID_ARG on a NULL table, loader, device or allocator; XUD_OK with 0 on a table with no FONT array (nothing of this kind
/// was ever created, which is a count of zero and not an error); XUD_UNSUPPORTED on a closed loader, on a device that is
/// not this loader's, or on a guard that cannot run; the guard runs once per font rather than once for the walk, because
/// the guard belongs to each atlas being destroyed rather than to the walk. A font record that holds an entry of set
/// 1's sampler array gives that entry back to the descriptor's free list before the atlas's close destroys the view and
/// sampler the entry names, the model rung's and the texture rung's own order, and a refusal there is reported in the
/// rung's own result rather than left behind.
XudResult xud_font_close_all(
    XudHandleTable* table, XudLoader* loader, const XudDevice* device, XudDescriptor* descriptor, XudAllocator* alloc, uint32_t* outClosed);

#endif /* XUD_XUD_FONT_H_ */
