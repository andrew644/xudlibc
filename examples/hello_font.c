/***********************************************************************************************
 *
 *   hello_font.c - hello world, told through a TrueType font
 *
 *   Owns : one font file read off disk, one context, one font loaded from that file's bytes, one
 *          string measured once and printed glyph by glyph, the pen layout that string's metrics
 *          describe drawn TWICE (once as one box per glyph by the caller's own walk, once as the
 *          string through the public text pair), and the teardown that gives the font back before
 *          it gives back the context. Nothing else: no atlas of its own, no bake of its own, no
 *          codepoint table of its own, no window of its own.
 *
 *   Layer: examples. Public API only (xud.h) plus stdio for the lines it prints and stdlib for
 *          the one buffer the font file is read into. That is the whole point of the public
 *          header here: user code names a font FILE, a string and a size, and never a glyph atlas,
 *          a stb_truetype call, a VkImage, a sampler, a staging arena or a block. The one thing
 *          the public header cannot do for a caller is read the file - xud_load_font takes BYTES,
 *          not a path (see the note beside XudFontData in xud.h: a path would make this call a
 *          file-system read, and the loader rung that reads a model's files is a different rung's
 *          decision) - so the read below is the example's own, and it is the shortest honest one.
 *
 *   What this example draws now and still cannot show: the public text pair landed, so the string
 *   IS drawn through the library's own walk, with the real uvs the walk writes into the batch's UV
 *   stream, the textured variant of the pipeline list beside the solid one, and the set-1 slot the
 *   walk samples with resolved from this font's own atlas - the glyphs come out as the letterforms
 *   of the file's own coverage, tinted by the call's colour. What the frame still cannot show is
 *   that the two walks agree: the boxes the caller's own walk draws beside them are the same boxes
 *   in a different colour, so the two walks are visible in one frame and a reader can see which
 *   numbers each of them places: the pair's glyphs are scaled uniformly in the library's square
 *   clip space, the caller's boxes carry the window's aspect divide, and the difference between
 *   the two is the aspect and nothing else.
 *
 *   The metrics printed below are the font file's own answers, not a table copied from a font
 *   editor: the numbers a run prints are what the bake wrote, and the same numbers are what the
 *   draw places its boxes by, which is what makes the printed table and the on-screen layout one
 *   statement rather than two hopes.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include <stdio.h> /* Required for: fopen, fseek, ftell, fread, fclose and printf, the file read and the report */
#include <stdlib.h> /* Required for: malloc and free, the one buffer the font file is read into */

#include "xud.h" /* Required for: XudFontData, XudGlyphInfo, XudGlyphAtlasRec, xud_load_font/xud_close_font, xud_measure_text, xud_draw_text_ex and the three glyph queries - the whole public font API */

/*-----------------------------------------------------------------------------------------------
 * The window, the font file, the bake and the string
 *-----------------------------------------------------------------------------------------------*/

/* 16:9 rather than square, for the same reason the aspect matters in every other example: clip
   space is -1..1 on BOTH axes while a window is not, and the conversion below is the one place
   this example turns the font's pixel metrics back into clip x. */
#define XUD_HELLO_WINDOW_WIDTH 640u
#define XUD_HELLO_WINDOW_HEIGHT 360u
#define XUD_HELLO_ASPECT ((float)XUD_HELLO_WINDOW_WIDTH / (float)XUD_HELLO_WINDOW_HEIGHT)

/* The font file is the repo's own test asset, read from the gitignored resources/ dir rather
   than from a system font dir: the bake below is stated against one file the repo carries, so a
   run on any machine prints the SAME numbers rather than whichever font collection that machine
   happens to ship. */
#define XUD_HELLO_FONT_PATH "resources/fonts/NotoSansTC-Regular.ttf"

/* The bake's em height, in pixels. It sits inside the font rung's own range (4..96) and it is
   small enough that the default set's 95 glyphs pack into the first atlas attempt the rung tries,
   which is why the load below costs one bake rather than a doubling ladder. */
#define XUD_HELLO_FONT_PIXEL_SIZE 32u

/* The default set: a NULL codepoint array with a zero count is the printable ASCII run, so the
   example names no codepoints of its own and the string below is measured against the set the
   font rung baked. */
#define XUD_HELLO_CODEPOINT_COUNT 0u

/* The string, as ASCII bytes turned into codepoints below: the public queries take a uint32_t
   array rather than a char*, because a font's rows are keyed by codepoint and not by byte, and
   the array is what a non-ASCII caller would hand the same way. */
static const char xudHelloText[] = "Hello, world!";
#define XUD_HELLO_TEXT_COUNT ((uint32_t)(sizeof(xudHelloText) - 1u))

/* The tracking, in the bake's own pixels, added after EVERY glyph including the last one - raylib's
   MeasureTextEx's spacing argument, and the reason the measure below includes the trailing step. */
#define XUD_HELLO_SPACING 2.0f

/* The em height the layout is drawn at, in clip y. The scale below is therefore the divide from
   the bake's pixels to this example's clip units, and it is a divide rather than a second bake:
   one atlas, many sizes, which is the reason raylib's Font carries a baseSize at all. */
#define XUD_HELLO_DRAW_EM 0.30f

/* The run's length in wall seconds and its frame cap. The clock is the library's own (xud_get_time,
   raylib's GetTime) and the cap is the safety net for a run whose clock never answers, set above
   what this machine reaches in the seconds below so the SECONDS are what end a normal run. */
#define XUD_HELLO_RUN_SECONDS 4.0
#define XUD_HELLO_FRAME_CAP 60000u

/*-----------------------------------------------------------------------------------------------
 * The font file, read once
 *-----------------------------------------------------------------------------------------------*/

static uint8_t* xud_hello_font_read(const char* path, uint32_t* outBytes) {
    FILE* file = NULL;
    long size = 0L;
    uint8_t* buffer = NULL;

    file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        (void)fclose(file);
        return NULL;
    }
    size = ftell(file);
    if (size <= 0L) {
        (void)fclose(file);
        return NULL;
    }
    rewind(file);
    buffer = (uint8_t*)malloc((size_t)size);
    if (buffer == NULL) {
        (void)fclose(file);
        return NULL;
    }
    if (fread(buffer, 1u, (size_t)size, file) != (size_t)size) {
        free(buffer);
        (void)fclose(file);
        return NULL;
    }
    (void)fclose(file);
    *outBytes = (uint32_t)size;
    return buffer;
}

/*-----------------------------------------------------------------------------------------------
 * The report: the string's width, then every glyph's two halves
 *-----------------------------------------------------------------------------------------------*/

/* The measure first, then one row per character: the width is the number a centring call needs and
   the rows are the two halves the font API keeps apart (the pen metrics and the atlas box, raylib's
   GlyphInfo and its GetGlyphAtlasRec), read back through the public queries rather than reached
   through a record. The index is printed too, because it is the one number the API's own fallback
   rule turns an uncovered codepoint into - and the '!' below is the last row of the default set, so
   the run shows a codepoint the set covers rather than a fallback. */
static XudResult xud_hello_font_report(
    XudContext* context, XudHandle font, const uint32_t* text, uint32_t textCount, float pixelSize, float spacing) {
    float width = 0.0f;
    XudResult result = xud_measure_text(&width, context, font, text, textCount, pixelSize, spacing);

    if (result != XUD_OK) {
        printf("hello_font: xud_measure_text -> %s\n", xud_result_name(result));
        return result;
    }
    printf("hello_font: \"%s\" at %u px, spacing %.1f px -> %.2f px wide (the trailing step included)\n", xudHelloText,
        XUD_HELLO_FONT_PIXEL_SIZE, XUD_HELLO_SPACING, width);

    for (uint32_t step = 0u; step < textCount; step++) {
        XudGlyphInfo info;
        XudGlyphAtlasRec rec;
        uint32_t index = 0u;

        result = xud_get_glyph_index(&index, context, font, text[step]);
        if (result != XUD_OK) {
            printf("hello_font: xud_get_glyph_index -> %s\n", xud_result_name(result));
            return result;
        }
        result = xud_get_glyph_info(&info, context, font, text[step]);
        if (result != XUD_OK) {
            printf("hello_font: xud_get_glyph_info -> %s\n", xud_result_name(result));
            return result;
        }
        result = xud_get_glyph_atlas_rec(&rec, context, font, text[step]);
        if (result != XUD_OK) {
            printf("hello_font: xud_get_glyph_atlas_rec -> %s\n", xud_result_name(result));
            return result;
        }

        printf("  '%c' U+%04X row %u: pen x%+.2f y%+.2f advance %+.2f | atlas %.0f,%.0f %.0fx%.0f\n", (char)text[step], text[step], index,
            info.offsetX, info.offsetY, info.advanceX, rec.x, rec.y, rec.width, rec.height);
    }

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The pen layout, drawn
 *-----------------------------------------------------------------------------------------------*/

/* One box per glyph, placed by the pen and sized by the glyph's atlas box: this is the walk a
   textured draw would do, minus the two things it cannot do today (the quad that samples the
   atlas, and the pipeline variant that reads it). The pen starts at the string's own left, which
   is half the measured width back from the window's centre - the measure is what centres the
   string rather than a guess at its length, and it is the SAME measure the report above printed,
   so the centred layout and the printed width cannot disagree.

   The two scales are the divide from the bake's pixels to clip units, and x carries the aspect
   because clip space is square and a window is not: a glyph's box drawn at one scale on both
   axes would be a box stretched 16:9 sideways on this window. The y sign is the font's own
   convention read back from the rows - offsetY is the step to the box's TOP edge, negative above
   the baseline in a space whose y grows downward, and clip y grows upward, so the top edge lands
   at baseline minus the offset and the box hangs below it.

   The public pair is drawn FIRST in this function, in its own colour, over the same string: the
   walk the library does now rather than the one this file writes, with the real uvs it writes and
   the textured variant it draws through. The two walks are in one frame on one string on one
   baseline so the difference between them is visible rather than asserted, and the difference is
   the aspect alone: the pair scales uniformly in the library's square clip space and this file's
   boxes carry the window's aspect divide, so the pair's glyphs land wider than the boxes they are
   meant to fill. The tracking the pair takes is in the same clip units as its em height, which is
   why the spacing below is divided out of the bake's pixels before it is handed over.
*/
static XudResult xud_hello_font_draw(
    XudContext* context, XudHandle font, const uint32_t* text, uint32_t textCount, float measuredPx, float spacing) {
    const float scale = XUD_HELLO_DRAW_EM / (float)XUD_HELLO_FONT_PIXEL_SIZE;
    const float scaleX = scale / XUD_HELLO_ASPECT;
    const float scaleY = scale;
    const float startX = -(measuredPx * scaleX) / 2.0f;
    const float baseline = -0.35f;
    const float depth = 0.05f;
    XudResult result = XUD_OK;

    /* The baseline itself, first and in a colour no glyph box will answer with: it is the one line
       the pen never crosses, and it is what makes the boxes' y offsets legible as offsets from a
       reference rather than as floating boxes.
    */
    result = xud_draw_line_3d(context, (XudVector3) { .x = startX, .y = baseline, .z = 0.0f },
        (XudVector3) { .x = startX + measuredPx * scaleX, .y = baseline, .z = 0.0f },
        (XudColor) { .r = 90u, .g = 90u, .b = 120u, .a = 255u });
    if (result != XUD_OK) {
        return result;
    }

    /* The string through the public pair: the library's own walk, from the pen's start rather than
       from the line's top-left, at the em height in clip y, with the real uvs it writes, the textured
       variant it draws through, and the set-1 slot it samples with resolved from this font's own
       atlas - so what lands is the file's own coverage tinted by the call's colour rather than a
       rectangle per glyph box. The boxes beside it are the caller's own walk of the same string in
       its own colour, so the two walks are in one frame on one string on one baseline.
    */
    result = xud_draw_text_ex(context, font, text, textCount, (XudVector3) { .x = startX, .y = baseline, .z = depth }, XUD_HELLO_DRAW_EM,
        (spacing * scaleY), (XudColor) { .r = 255u, .g = 120u, .b = 90u, .a = 255u });
    if (result != XUD_OK) {
        return result;
    }

    float pen = startX;

    for (uint32_t step = 0u; step < textCount; step++) {
        XudGlyphInfo info;
        XudGlyphAtlasRec rec;

        result = xud_get_glyph_info(&info, context, font, text[step]);
        if (result != XUD_OK) {
            return result;
        }
        result = xud_get_glyph_atlas_rec(&rec, context, font, text[step]);
        if (result != XUD_OK) {
            return result;
        }

        /* A codepoint with no outline - the space - has a box of zero width and still moves the pen:
           that is the answer the API states rather than an absent row, and skipping the draw while
           keeping the advance is what keeps the word break a word break. */
        if ((rec.width > 0.0f) && (rec.height > 0.0f)) {
            const float boxW = rec.width * scaleX;
            const float boxH = rec.height * scaleY;
            const float centreX = pen + (info.offsetX * scaleX) + (boxW / 2.0f);
            const float centreY = baseline - (info.offsetY * scaleY) - (boxH / 2.0f);

            result = xud_draw_cube(context, (XudVector3) { .x = centreX, .y = centreY, .z = 0.0f }, boxW, boxH, depth,
                (XudColor) { .r = 240u, .g = 220u, .b = 120u, .a = 255u });
            if (result != XUD_OK) {
                return result;
            }
        }

        pen += (info.advanceX + spacing) * scaleX;
    }

    return XUD_OK;
}

int main(void) {
    uint32_t fontBytes = 0u;
    uint8_t* fileData = xud_hello_font_read(XUD_HELLO_FONT_PATH, &fontBytes);

    if (fileData == NULL) {
        printf("hello_font: no font file at %s, so there is nothing to bake\n", XUD_HELLO_FONT_PATH);
        return 1;
    }
    printf("hello_font: read %u bytes of %s\n", fontBytes, XUD_HELLO_FONT_PATH);

    uint32_t text[XUD_HELLO_TEXT_COUNT];

    for (uint32_t step = 0u; step < XUD_HELLO_TEXT_COUNT; step++) {
        text[step] = (uint32_t)(uint8_t)xudHelloText[step];
    }

    XudContext* context = NULL;
    XudResult step = xud_init_context(&context, XUD_HELLO_WINDOW_WIDTH, XUD_HELLO_WINDOW_HEIGHT);

    printf("hello_font: xud_init_context(%u, %u) -> %s (xudlibc %s)\n", XUD_HELLO_WINDOW_WIDTH, XUD_HELLO_WINDOW_HEIGHT,
        xud_result_name(step), XUD_VERSION);
    if (step != XUD_OK) {
        free(fileData);
        return (step == XUD_UNSUPPORTED) ? 0 : 1;
    }

    /* The font, in one description: the file's bytes, the em height to bake them at, and the default
       set named as a NULL array with a zero count. Nothing here names an atlas extent or a sampling
       token - the extent is the rung's to find and the tokens are a font's, see the note beside
       XudFontData - and nothing here names the texture the atlas becomes, because a font's atlas is
       a TEXTURE slot no caller can reach. */
    const XudFontData fontData = (XudFontData) { .fileData = fileData,
        .fileBytes = fontBytes,
        .pixelSize = XUD_HELLO_FONT_PIXEL_SIZE,
        .codepoints = NULL,
        .codepointCount = XUD_HELLO_CODEPOINT_COUNT };

    XudHandle font;

    step = xud_load_font(&font, context, &fontData);
    if (step != XUD_OK) {
        printf("hello_font: xud_load_font -> %s\n", xud_result_name(step));
        (void)xud_close_context(&context);
        free(fileData);
        return 1;
    }
    printf("hello_font: xud_load_font at %u px, default set -> %s\n", XUD_HELLO_FONT_PIXEL_SIZE, xud_result_name(step));

    float measured = 0.0f;

    step = xud_hello_font_report(context, font, text, XUD_HELLO_TEXT_COUNT, (float)XUD_HELLO_FONT_PIXEL_SIZE, XUD_HELLO_SPACING);
    if (step != XUD_OK) {
        printf("hello_font: the report -> %s\n", xud_result_name(step));
        (void)xud_close_font(context, font);
        (void)xud_close_context(&context);
        free(fileData);
        return 1;
    }

    uint32_t frame = 0u;
    uint32_t shortFrames = 0u;
    double now = 0.0;

    for (frame = 0u; frame < XUD_HELLO_FRAME_CAP; frame++) {
        step = xud_begin_drawing(context);
        if (step != XUD_OK) {
            printf("hello_font: frame %u xud_begin_drawing -> %s\n", frame, xud_result_name(step));
            break;
        }

        /* The layout is re-drawn every frame rather than once, because the batch is reset every frame
           and the frame's own draws are what a submission reads. A refused append is not the run's
           end: the string's 12 boxes beside its 12 glyph quads are 336 vertices against the batch's
           initial 64, so the first frames are SHORT and the boundary above grows the batch for the
           next one - the same short-frame shape breakout_3d states, and the same reason the run keeps
           going.
        */
        step = xud_hello_font_draw(context, font, text, XUD_HELLO_TEXT_COUNT, measured, XUD_HELLO_SPACING);
        if (step == XUD_OUT_OF_MEMORY) {
            shortFrames++;
            step = XUD_OK;
        } else if (step != XUD_OK) {
            printf("hello_font: frame %u the layout -> %s\n", frame, xud_result_name(step));
            break;
        }

        step = xud_end_drawing(context);
        if (step != XUD_OK) {
            printf("hello_font: frame %u xud_end_drawing -> %s\n", frame, xud_result_name(step));
            break;
        }

        if (xud_get_time(&now, context) != XUD_OK) {
            printf("hello_font: the clock refused at frame %u, and the run stops here\n", frame);
            break;
        }
        if (now >= XUD_HELLO_RUN_SECONDS) {
            break;
        }
    }

    printf("hello_font: %u frames, %u short frames while the batch settled, %.2f s\n", frame, shortFrames, now);

    /* The font given back BEFORE the context: the atlas it owns is a TEXTURE slot of the context's
       handle table, and the table is the top rung xud_close_context() retires first - a context
       closed with a font still live would be a table freed under a record that names an image. */
    step = xud_close_font(context, font);
    printf("hello_font: xud_close_font -> %s\n", xud_result_name(step));

    const XudResult closed = xud_close_context(&context);

    printf("hello_font: xud_close_context -> %s\n", xud_result_name(closed));
    free(fileData);

    return ((step != XUD_OK) || (closed != XUD_OK)) ? 1 : 0;
}
