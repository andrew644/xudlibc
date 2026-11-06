/***********************************************************************************************
 *
 *   hello_ui.c - hello world, told through an immediate-mode button
 *
 *   Owns : one font file read off disk, one context, one ui rung opened on that font, and a frame
 *          loop that feeds the context's input into the ui, draws a window with a button and a
 *          progress bar against the nuklear context, and folds the frame into the batch. Nothing
 *          else: no atlas of its own (the rung bakes the font), no pipeline of its own (the fold
 *          draws through the existing TEXTURED row), no scissor of its own (the windows overlap by
 *          paint order).
 *
 *   Layer: examples. Public API only (xud.h) plus stdio for the lines it prints and stdlib for the
 *          one buffer the font file is read into. The caller names a font file and a window extent,
 *          and the widgets are written against the nuklear context xud_ui_get_context() hands back
 *          - the gui is a first-class rung, not an opaque handle, so this example includes the
 *          nuklear widget calls the way it includes the rest of the public API.
 *
 *   What this example draws: a titled window holding a button that flips a counter and a progress
 *          bar that fills with it, both re-drawn every frame against the same context. The button
 *          answers the mouse the context's own input rung fed it, and the fold lands the frame in
 *          the batch the same way the font walk does - one draw row, the atlas's one texture.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include <stdio.h> /* Required for: fopen, fseek, ftell, fread, fclose and printf, the file read and the report */
#include <stdlib.h> /* Required for: malloc and free, the one buffer the font file is read into */

#include "xud.h" /* Required for: the context pair, the ui rung's five calls, and the nuklear widget API they frame */

/*-----------------------------------------------------------------------------------------------
 * The window, the font file and the run
 *-----------------------------------------------------------------------------------------------*/

#define XUD_HELLO_WINDOW_WIDTH 640u
#define XUD_HELLO_WINDOW_HEIGHT 360u

/* The font file is the repo's own test asset, read from the gitignored resources/ dir: the bake
   below is stated against one file the repo carries, so a run on any machine shows the same
   letterforms rather than whichever font collection that machine happens to ship. */
#define XUD_HELLO_FONT_PATH "resources/fonts/NotoSansTC-Regular.ttf"
#define XUD_HELLO_FONT_PIXEL_SIZE 16u

/* The run's length in wall seconds and its frame cap, the same safety net every example carries:
   the seconds are what end a normal run and the cap is the net for a clock that never answers. */
#define XUD_HELLO_RUN_SECONDS 4.0
#define XUD_HELLO_FRAME_CAP 60000u

/*-----------------------------------------------------------------------------------------------
 * The font file, read once
 *-----------------------------------------------------------------------------------------------*/

static uint8_t* xud_hello_ui_font_read(const char* path, uint32_t* outBytes) {
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

int main(void) {
    uint32_t fontBytes = 0u;
    uint8_t* fileData = xud_hello_ui_font_read(XUD_HELLO_FONT_PATH, &fontBytes);

    if (fileData == NULL) {
        printf("hello_ui: no font file at %s, so there is nothing to bake\n", XUD_HELLO_FONT_PATH);
        return 1;
    }
    printf("hello_ui: read %u bytes of %s\n", fontBytes, XUD_HELLO_FONT_PATH);

    XudContext* context = NULL;
    XudResult step = xud_init_context(&context, XUD_HELLO_WINDOW_WIDTH, XUD_HELLO_WINDOW_HEIGHT);

    printf("hello_ui: xud_init_context(%u, %u) -> %s (xudlibc %s)\n", XUD_HELLO_WINDOW_WIDTH, XUD_HELLO_WINDOW_HEIGHT,
        xud_result_name(step), XUD_VERSION);
    if (step != XUD_OK) {
        free(fileData);
        return (step == XUD_UNSUPPORTED) ? 0 : 1;
    }

    /* The ui rung, opened on the font's bytes: the rung bakes the file into one atlas image, opens
       that image as a TEXTURE and names it on every glyph, so the caller never names an atlas or a
       sampler - the same "name a file, not a texture" shape the font pair carries. */
    const XudFontData fontData = (XudFontData) {
        .fileData = fileData, .fileBytes = fontBytes, .pixelSize = XUD_HELLO_FONT_PIXEL_SIZE, .codepoints = NULL, .codepointCount = 0u
    };

    step = xud_ui_init(context, &fontData);
    if (step != XUD_OK) {
        printf("hello_ui: xud_ui_init -> %s\n", xud_result_name(step));
        (void)xud_close_context(&context);
        free(fileData);
        return 1;
    }
    printf("hello_ui: xud_ui_init -> %s\n", xud_result_name(step));

    struct nk_context* nk = xud_ui_get_context(context);
    if (nk == NULL) {
        printf("hello_ui: no nuklear context behind the open rung\n");
        (void)xud_ui_close(context);
        (void)xud_close_context(&context);
        free(fileData);
        return 1;
    }

    /* The widget state the frame loop carries: the button's count and the progress bar's fill it
       drives. Both live in the example rather than the rung - the rung owns the context and the
       atlas, and the caller owns the widgets it draws against it. nk_progress names its value in
       nk_size units, so the fill is an integer 0..100 rather than a float. */
    int clicks = 0;
    nk_size progress = 0u;

    uint32_t frame = 0u;
    uint32_t shortFrames = 0u;
    double now = 0.0;

    for (frame = 0u; frame < XUD_HELLO_FRAME_CAP; frame++) {
        step = xud_begin_drawing(context);
        if (step != XUD_OK) {
            printf("hello_ui: frame %u xud_begin_drawing -> %s\n", frame, xud_result_name(step));
            break;
        }

        /* The input, fed from the context's own input rung into the nuklear context before the
           widgets read it: the keys, the mouse and the wheel the frame below's button answers are
           the state this call wrote. */
        step = xud_ui_begin(context);
        if (step != XUD_OK) {
            printf("hello_ui: frame %u xud_ui_begin -> %s\n", frame, xud_result_name(step));
            break;
        }

        /* The frame's widgets, written against the context the rung handed back: one window holding
           a button that flips the count on a click and a progress bar that fills with it. nk_begin()
           opens the window and nk_end() closes it - the pair that frames the frame xud_ui_draw()
           converts. The button's nk_button_label() is what reads the input xud_ui_begin fed. */
        if (nk_begin(nk, "hello_ui", nk_rect(20, 20, 320, 160), NK_WINDOW_TITLE | NK_WINDOW_BORDER)) {
            char label[32];
            snprintf(label, sizeof(label), "clicks: %d", clicks);
            if (nk_button_label(nk, label)) {
                clicks++;
                progress = (nk_size)clicks * 10u;
                if (progress > 100u) {
                    progress = 100u;
                }
            }
            nk_layout_row_dynamic(nk, 20, 1);
            nk_progress(nk, &progress, 100u, nk_false);
        }
        nk_end(nk);

        /* The fold: convert the frame into the staging and fold it into the batch at the context's
           own extent, through the atlas's one TEXTURE slot. A frame that does not fit the batch is
           deferred as one unit, so a short frame is not the run's end - the boundary above grows the
           batch for the next frame and the ui reappears, the same short-frame shape the other
           examples state. */
        step = xud_ui_draw(context);
        if (step == XUD_OUT_OF_MEMORY) {
            shortFrames++;
            step = XUD_OK;
        } else if (step != XUD_OK) {
            printf("hello_ui: frame %u xud_ui_draw -> %s\n", frame, xud_result_name(step));
            break;
        }

        step = xud_end_drawing(context);
        if (step != XUD_OK) {
            printf("hello_ui: frame %u xud_end_drawing -> %s\n", frame, xud_result_name(step));
            break;
        }

        if (xud_get_time(&now, context) != XUD_OK) {
            printf("hello_ui: the clock refused at frame %u, and the run stops here\n", frame);
            break;
        }
        if (now >= XUD_HELLO_RUN_SECONDS) {
            break;
        }
    }

    printf("hello_ui: %u frames, %u short frames while the batch settled, %.2f s, %d clicks\n", frame, shortFrames, now, clicks);

    /* The ui given back BEFORE the context: the atlas it owns is a TEXTURE slot of the context's
       handle table, and the table is the top rung xud_close_context() retires first - though
       xud_close_context() closes the ui first of all now, so a caller that closes the context
       without the ui still gets a clean teardown. */
    step = xud_ui_close(context);
    printf("hello_ui: xud_ui_close -> %s\n", xud_result_name(step));

    const XudResult closed = xud_close_context(&context);

    printf("hello_ui: xud_close_context -> %s\n", xud_result_name(closed));
    free(fileData);

    return ((step != XUD_OK) || (closed != XUD_OK)) ? 1 : 0;
}
