/***********************************************************************************************
 *
 *   xud_ui.c - the ui rung: one nuklear immediate-mode context, its one baked font atlas and the
 *              fold of a nuklear frame's converted vertices into the batch
 *
 *   Owns : the implementation of xud_ui.h - the three pure halves (the vertex mapper, the
 *          draw-list -> batch fold and the input feed) and the three driver halves (open, close
 *          and the two frame halves).
 *   Layer: above the batch, the texture, the descriptor and the input rungs; the one .c in the
 *          tree that includes xud_ui.h's nuklear declarations alongside the context, the texture
 *          and the batch rungs it drives.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include "xud_ui.h" /* Required for: XudUi, XudUiVertex, the pure halves and the driver halves this file implements, and the nuklear declarations they are written against */

#include <stdlib.h> /* Required for: malloc / calloc / free, the rung's own host allocation */
#include <string.h> /* Required for: memset, the zeroed convert config */

#include "xud_core.h" /* Required for: struct XudContext, whose input, renderer, extent and bring-up rungs the driver halves read */
#include "xud_texture.h" /* Required for: xud_create_texture / xud_close_texture / xud_texture_get, the atlas image's open, close and slot read */

/*-----------------------------------------------------------------------------------------------
 * The input feed's key helpers
 *-----------------------------------------------------------------------------------------------*/

/* One key's down state, read off the input rung's current state row. The index is the token
   minus the row's first token, the same arithmetic the input rung's own queries do, so a token
   outside the row's range reads up rather than past the row. */
static nk_bool xud_ui_key_down(const XudInput* input, int32_t token) {
    uint32_t index;

    if ((int32_t)token < XUD_INPUT_KEY_FIRST) {
        return nk_false;
    }
    index = (uint32_t)(token - XUD_INPUT_KEY_FIRST);
    if (index >= XUD_INPUT_KEY_COUNT) {
        return nk_false;
    }
    return (input->keyState[index] != XUD_INPUT_STATE_UP) ? nk_true : nk_false;
}

/* One mouse button's down state, the button row read the same way the input rung reads it. */
static nk_bool xud_ui_button_down(const XudInput* input, uint32_t index) {
    if (index >= XUD_INPUT_BUTTON_COUNT) {
        return nk_false;
    }
    return (input->buttonState[index] != XUD_INPUT_STATE_UP) ? nk_true : nk_false;
}

/*-----------------------------------------------------------------------------------------------
 * The pure halves
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_ui_map_vertex(
    const XudUiVertex* vertex, uint32_t fbWidth, uint32_t fbHeight, float outPos[3], float outUv[2], uint8_t outColor[4]) {
    if ((vertex == NULL) || (outPos == NULL) || (outUv == NULL) || (outColor == NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((fbWidth == 0u) || (fbHeight == 0u)) {
        return XUD_INVALID_ARG;
    }

    /* The pixel (y down) to the y-up clip space the identity matrices expect, the inverse of the
       vertex stage's own final Y-flip: the top of the framebuffer maps to +1 and the bottom to -1. */
    outPos[0] = (2.0f * vertex->pos[0] / (float)fbWidth) - 1.0f;
    outPos[1] = 1.0f - (2.0f * vertex->pos[1] / (float)fbHeight);
    outPos[2] = 0.0f;
    outUv[0] = vertex->uv[0];
    outUv[1] = vertex->uv[1];
    outColor[0] = vertex->col[0];
    outColor[1] = vertex->col[1];
    outColor[2] = vertex->col[2];
    outColor[3] = vertex->col[3];

    return XUD_OK;
}

XudResult xud_ui_append_draw_list(XudBatch* batch, const XudUiVertex* vertices, uint32_t vertexCount, const uint16_t* elements,
    uint32_t elementCount, uint32_t fbWidth, uint32_t fbHeight, uint32_t atlasSlot) {
    XudResult result = XUD_INVALID_ARG;
    uint32_t oldVariant = 0u, oldMaterial = 0u, oldTexture = 0u;
    uint32_t liveVertices = 0u, liveIndices = 0u, vertexCapacity = 0u, drawCount = 0u;
    uint32_t indexCapacity = 0u;
    uint32_t i = 0u;
    VkDeviceSize stagingBytes = 0u;

    if ((batch == NULL) || ((vertexCount > 0u) && (vertices == NULL)) || ((elementCount > 0u) && (elements == NULL))) {
        return XUD_INVALID_ARG;
    }
    if ((fbWidth == 0u) || (fbHeight == 0u)) {
        return XUD_INVALID_ARG;
    }
    if (vertexCount == 0u) {
        return XUD_OK;
    }

    result = xud_batch_stats(batch, &liveVertices, &liveIndices, &vertexCapacity, &drawCount, &stagingBytes);
    if (result != XUD_OK) {
        return result;
    }
    indexCapacity = XUD_BATCH_INDEX_CAPACITY(vertexCapacity);

    /* A frame that does not fit is deferred as one unit, not truncated: the ask is noted for the
       boundary's grow and nothing is appended, so the ui simply reappears the frame after the
       batch has grown rather than drawing a half window. */
    if (((liveVertices + vertexCount) > vertexCapacity) || ((liveIndices + elementCount) > indexCapacity)) {
        (void)xud_batch_note_refused(batch, liveVertices + vertexCount, liveIndices + elementCount);
        return XUD_OK;
    }

    /* The state move, before a byte is written: the whole frame draws through the textured row of
       the atlas's one slot, and the state the caller found is restored at the end, which closes
       the run into one draw row of that row. */
    result = xud_batch_state_get(batch, &oldVariant, &oldMaterial, &oldTexture);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_batch_set_material(batch, XUD_BATCH_NO_MATERIAL, XUD_BATCH_VARIANT_TEXTURED, atlasSlot);
    if (result != XUD_OK) {
        return result;
    }

    for (i = 0u; i < vertexCount; i++) {
        float pos[3];
        float uv[2];
        uint8_t color[4];
        float normal[3];
        normal[0] = 0.0f;
        normal[1] = 1.0f;
        normal[2] = 0.0f;

        if (xud_ui_map_vertex(&vertices[i], fbWidth, fbHeight, pos, uv, color) != XUD_OK) {
            (void)xud_batch_set_material(batch, oldMaterial, oldVariant, oldTexture);
            return XUD_INVALID_ARG;
        }
        result = xud_batch_append_vertex(batch, pos, uv, color, normal);
        if (result != XUD_OK) {
            (void)xud_batch_set_material(batch, oldMaterial, oldVariant, oldTexture);
            return result;
        }
    }

    /* The vertices were appended at the live count, so that count is the base every element is
       shifted by: nk's indices are 0-based over its own staging, and the batch's are 0-based over
       the whole arena. */
    for (i = 0u; i < elementCount; i++) {
        result = xud_batch_append_index(batch, liveVertices + (uint32_t)elements[i]);
        if (result != XUD_OK) {
            (void)xud_batch_set_material(batch, oldMaterial, oldVariant, oldTexture);
            return result;
        }
    }

    result = xud_batch_set_material(batch, oldMaterial, oldVariant, oldTexture);
    if (result != XUD_OK) {
        return result;
    }

    return XUD_OK;
}

XudResult xud_ui_feed_input(struct nk_context* context, const XudInput* input) {
    int32_t cursorX = 0;
    int32_t cursorY = 0;
    nk_bool shift;
    nk_bool control;
    nk_bool alt;
    uint32_t i = 0u;

    if ((context == NULL) || (input == NULL)) {
        return XUD_INVALID_ARG;
    }

    cursorX = (int32_t)input->cursorX;
    cursorY = (int32_t)input->cursorY;
    shift = xud_ui_key_down(input, XUD_KEY_LEFT_SHIFT) || xud_ui_key_down(input, XUD_KEY_RIGHT_SHIFT);
    control = xud_ui_key_down(input, XUD_KEY_LEFT_CONTROL) || xud_ui_key_down(input, XUD_KEY_RIGHT_CONTROL);
    alt = xud_ui_key_down(input, XUD_KEY_LEFT_ALT) || xud_ui_key_down(input, XUD_KEY_RIGHT_ALT);

    nk_input_begin(context);

    for (i = 0u; i < input->charQueueCount; i++) {
        nk_input_unicode(context, (nk_rune)input->charQueue[i]);
    }

    nk_input_motion(context, cursorX, cursorY);

    nk_input_key(context, NK_KEY_SHIFT, shift);
    nk_input_key(context, NK_KEY_CTRL, control);
    nk_input_key(context, NK_KEY_ALT, alt);
    nk_input_key(context, NK_KEY_DEL, xud_ui_key_down(input, XUD_KEY_DELETE));
    nk_input_key(context, NK_KEY_ENTER, xud_ui_key_down(input, XUD_KEY_ENTER));
    nk_input_key(context, NK_KEY_TAB, xud_ui_key_down(input, XUD_KEY_TAB));
    nk_input_key(context, NK_KEY_BACKSPACE, xud_ui_key_down(input, XUD_KEY_BACKSPACE));
    nk_input_key(context, NK_KEY_TEXT_START, xud_ui_key_down(input, XUD_KEY_HOME));
    nk_input_key(context, NK_KEY_TEXT_END, xud_ui_key_down(input, XUD_KEY_END));
    nk_input_key(context, NK_KEY_SCROLL_START, xud_ui_key_down(input, XUD_KEY_HOME));
    nk_input_key(context, NK_KEY_SCROLL_END, xud_ui_key_down(input, XUD_KEY_END));

    /* The context-sensitive keys read the way the reference backend reads them: under control the
       arrows and the letter keys are their text-editing meanings, and without it the arrows are
       plain navigation. */
    if (control != nk_false) {
        nk_input_key(context, NK_KEY_COPY, xud_ui_key_down(input, XUD_KEY_C));
        nk_input_key(context, NK_KEY_PASTE, xud_ui_key_down(input, XUD_KEY_V));
        nk_input_key(context, NK_KEY_CUT, xud_ui_key_down(input, XUD_KEY_X));
        nk_input_key(context, NK_KEY_TEXT_SELECT_ALL, xud_ui_key_down(input, XUD_KEY_A));
        nk_input_key(context, NK_KEY_TEXT_WORD_LEFT, xud_ui_key_down(input, XUD_KEY_LEFT));
        nk_input_key(context, NK_KEY_TEXT_WORD_RIGHT, xud_ui_key_down(input, XUD_KEY_RIGHT));
    } else {
        nk_input_key(context, NK_KEY_LEFT, xud_ui_key_down(input, XUD_KEY_LEFT));
        nk_input_key(context, NK_KEY_RIGHT, xud_ui_key_down(input, XUD_KEY_RIGHT));
    }
    nk_input_key(context, NK_KEY_UP, xud_ui_key_down(input, XUD_KEY_UP));
    nk_input_key(context, NK_KEY_DOWN, xud_ui_key_down(input, XUD_KEY_DOWN));

    nk_input_button(context, NK_BUTTON_LEFT, cursorX, cursorY, xud_ui_button_down(input, 0u));
    nk_input_button(context, NK_BUTTON_MIDDLE, cursorX, cursorY, xud_ui_button_down(input, 1u));
    nk_input_button(context, NK_BUTTON_RIGHT, cursorX, cursorY, xud_ui_button_down(input, 2u));

    nk_input_scroll(context, (struct nk_vec2) { (float)input->wheelX, (float)input->wheelY });

    nk_input_end(context);

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The driver halves
 *-----------------------------------------------------------------------------------------------*/

/* The one vertex layout nk_convert() writes into the staging, the reference backend's own. */
static const struct nk_draw_vertex_layout_element xud_ui_vertex_layout[] = {
    { NK_VERTEX_POSITION, NK_FORMAT_FLOAT, NK_OFFSETOF(XudUiVertex, pos) },
    { NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT, NK_OFFSETOF(XudUiVertex, uv) },
    { NK_VERTEX_COLOR, NK_FORMAT_R8G8B8A8, NK_OFFSETOF(XudUiVertex, col) },
    { NK_VERTEX_LAYOUT_END },
};

XudResult xud_ui_open(XudUi** outUi, XudContext* context, const XudFontData* font) {
    XudUi* ui = NULL;
    XudResult result = XUD_INVALID_ARG;
    const void* image = NULL;
    const XudTextureRecord* record = NULL;
    XudTextureData texture;
    int imageWidth = 0;
    int imageHeight = 0;

    if ((outUi == NULL) || (context == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (*outUi != NULL) {
        return XUD_INVALID_ARG;
    }
    if ((context->handleTable == NULL) || (context->loader == NULL) || (context->device == NULL) || (context->physicalDevice == NULL)
        || (context->allocator == NULL) || (context->descriptor == NULL)) {
        return XUD_UNSUPPORTED;
    }

    ui = (XudUi*)calloc(1u, sizeof(XudUi));
    if (ui == NULL) {
        return XUD_OUT_OF_MEMORY;
    }
    ui->context = context;
    ui->atlasTexture = XUD_HANDLE_NONE;

    /* Bake the caller's font (or the built-in default) into the atlas, then open that image as the
       context's one TEXTURE. The order is load-bearing: the create reads the baked bytes, and
       nk_font_atlas_end below is what frees them, so the create has to run first. */
    nk_font_atlas_init_default(&ui->atlas);
    nk_font_atlas_begin(&ui->atlas);
    if (font != NULL) {
        ui->font = nk_font_atlas_add_from_memory(&ui->atlas, (void*)font->fileData, (nk_size)font->fileBytes, (float)font->pixelSize, NULL);
    } else {
        ui->font = nk_font_atlas_add_default(&ui->atlas, 13.0f, NULL);
    }
    if (ui->font == NULL) {
        nk_font_atlas_clear(&ui->atlas);
        free(ui);
        return XUD_INTERNAL_ERROR;
    }
    image = nk_font_atlas_bake(&ui->atlas, &imageWidth, &imageHeight, NK_FONT_ATLAS_RGBA32);
    if ((image == NULL) || (imageWidth <= 0) || (imageHeight <= 0)) {
        nk_font_atlas_clear(&ui->atlas);
        free(ui);
        return XUD_INTERNAL_ERROR;
    }

    texture.width = (uint32_t)imageWidth;
    texture.height = (uint32_t)imageHeight;
    texture.rgba = (const uint8_t*)image;
    texture.filter = XUD_TEXTURE_FILTER_LINEAR;
    texture.addressMode = XUD_TEXTURE_ADDRESS_CLAMP_TO_EDGE;
    result = xud_create_texture(&ui->atlasTexture, context, &texture);
    if (result != XUD_OK) {
        nk_font_atlas_clear(&ui->atlas);
        free(ui);
        return result;
    }

    result = xud_texture_get(context->handleTable, ui->atlasTexture, &record);
    if (result != XUD_OK) {
        (void)xud_close_texture(context, ui->atlasTexture);
        nk_font_atlas_clear(&ui->atlas);
        free(ui);
        return result;
    }
    ui->atlasSlot = record->samplerSlot;

    /* Name the atlas's texture on every font and fill the null texture with the atlas's own white
       pixel: that is what makes glyph and shape commands both draw through the one slot. This call
       frees the baked image, which the create above has already read. */
    nk_font_atlas_end(&ui->atlas, nk_handle_id((int)ui->atlasSlot), &ui->texNull);

    ui->vertexStaging = (XudUiVertex*)malloc(XUD_UI_VERTEX_STAGING * sizeof(XudUiVertex));
    ui->elementStaging = (uint16_t*)malloc(XUD_UI_ELEMENT_STAGING * sizeof(uint16_t));
    if ((ui->vertexStaging == NULL) || (ui->elementStaging == NULL)) {
        (void)xud_close_texture(context, ui->atlasTexture);
        nk_font_atlas_clear(&ui->atlas);
        free(ui->vertexStaging);
        free(ui->elementStaging);
        free(ui);
        return XUD_OUT_OF_MEMORY;
    }

    nk_buffer_init_default(&ui->commands);
    if (nk_init_default(&ui->nk, 0) == nk_false) {
        (void)xud_close_texture(context, ui->atlasTexture);
        nk_font_atlas_clear(&ui->atlas);
        free(ui->vertexStaging);
        free(ui->elementStaging);
        nk_buffer_free(&ui->commands);
        free(ui);
        return XUD_INTERNAL_ERROR;
    }

    /* The context opened with no font; the atlas's font is what the widgets draw with, so it is
       set on the style here. The handle is the user-font wrapper nk_font_atlas_end named with the
       atlas's texture, so text and shapes both sample the one slot. */
    nk_style_set_font(&ui->nk, &ui->font->handle);

    *outUi = ui;
    return XUD_OK;
}

XudResult xud_ui_destroy(XudUi** ioUi, XudContext* context) {
    XudUi* ui = NULL;

    if (ioUi == NULL) {
        return XUD_INVALID_ARG;
    }
    ui = *ioUi;
    if (ui == NULL) {
        return XUD_ALREADY;
    }

    /* The reverse of open: the atlas's TEXTURE through the context's own close (which gives its
       set-1 slot back and retires the image, the view and the sampler), the nuklear context, the
       atlas, the command buffer and the staging, and the rung itself. */
    if ((context != NULL) && (!xud_handle_equal(ui->atlasTexture, XUD_HANDLE_NONE))) {
        (void)xud_close_texture(context, ui->atlasTexture);
    }
    nk_free(&ui->nk);
    nk_font_atlas_clear(&ui->atlas);
    nk_buffer_free(&ui->commands);
    free(ui->vertexStaging);
    free(ui->elementStaging);
    free(ui);
    *ioUi = NULL;

    return XUD_OK;
}

XudResult xud_ui_frame_begin(XudUi* ui) {
    if (ui == NULL) {
        return XUD_INVALID_ARG;
    }
    if (ui->context == NULL) {
        return XUD_UNSUPPORTED;
    }
    return xud_ui_feed_input(&ui->nk, ui->context->input);
}

XudResult xud_ui_frame_draw(XudUi* ui) {
    XudResult result = XUD_INVALID_ARG;
    struct nk_buffer vertexBuffer;
    struct nk_buffer elementBuffer;
    struct nk_convert_config config;
    uint32_t vertexCount = 0u;
    uint32_t elementCount = 0u;

    if (ui == NULL) {
        return XUD_INVALID_ARG;
    }
    if ((ui->context == NULL) || (ui->context->renderer == NULL) || (ui->context->renderer->batch == NULL)) {
        return XUD_UNSUPPORTED;
    }

    /* Convert the frame the caller's widgets opened and closed into the staging. The caller ran
       nk_begin() ... nk_end() against the context between xud_ui_begin() and this call, so the
       frame is already finalised here and this call does not end it again - a second nk_end() would
       close a window that is already closed. The command buffer is cleared first: it is the
       convert's output and it is what the fold below writes from, so a stale command from a frame
       that was refused is not one that gets converted twice. */
    nk_buffer_clear(&ui->commands);

    memset(&config, 0, sizeof(config));
    config.vertex_layout = xud_ui_vertex_layout;
    config.vertex_size = (nk_size)sizeof(XudUiVertex);
    config.vertex_alignment = (nk_size)NK_ALIGNOF(XudUiVertex);
    config.tex_null = ui->texNull;
    config.circle_segment_count = 22u;
    config.curve_segment_count = 22u;
    config.arc_segment_count = 22u;
    config.global_alpha = 1.0f;
    config.shape_AA = NK_ANTI_ALIASING_ON;
    config.line_AA = NK_ANTI_ALIASING_ON;

    nk_buffer_init_fixed(&vertexBuffer, ui->vertexStaging, XUD_UI_VERTEX_STAGING * sizeof(XudUiVertex));
    nk_buffer_init_fixed(&elementBuffer, ui->elementStaging, XUD_UI_ELEMENT_STAGING * sizeof(uint16_t));
    nk_convert(&ui->nk, &ui->commands, &vertexBuffer, &elementBuffer, &config);

    vertexCount = (uint32_t)(vertexBuffer.allocated / sizeof(XudUiVertex));
    elementCount = (uint32_t)(elementBuffer.allocated / sizeof(uint16_t));

    /* The fold into the batch at the context's own extent. The extent is the framebuffer's, which
       the mapper divides by; a window extent is the answer for a window that has not been resized
       into a different framebuffer. */
    result = xud_ui_append_draw_list(ui->context->renderer->batch, ui->vertexStaging, vertexCount, ui->elementStaging, elementCount,
        ui->context->width, ui->context->height, ui->atlasSlot);

    /* Reset the context for the next frame: nk_clear() frees the windows and the command queue the
       frame used, and the convert has read them, so nothing of this frame's widgets is still owed.
       The input the caller fed is reset too, which is why xud_ui_begin() re-feeds it every frame. */
    nk_clear(&ui->nk);

    return result;
}

/*-----------------------------------------------------------------------------------------------
 * The public wrappers, the five xud_ui_*() the context owns the rung through
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_ui_init(XudContext* context, const XudFontData* font) {
    if (context == NULL) {
        return XUD_INVALID_ARG;
    }
    if (context->ui != NULL) {
        return XUD_ALREADY;
    }
    return xud_ui_open(&context->ui, context, font);
}

XudResult xud_ui_close(XudContext* context) {
    if (context == NULL) {
        return XUD_INVALID_ARG;
    }
    return xud_ui_destroy(&context->ui, context);
}

struct nk_context* xud_ui_get_context(XudContext* context) {
    if (context == NULL) {
        return NULL;
    }
    if (context->ui == NULL) {
        return NULL;
    }
    return &context->ui->nk;
}

XudResult xud_ui_begin(XudContext* context) {
    if (context == NULL) {
        return XUD_INVALID_ARG;
    }
    if (context->ui == NULL) {
        return XUD_UNSUPPORTED;
    }
    return xud_ui_frame_begin(context->ui);
}

XudResult xud_ui_draw(XudContext* context) {
    if (context == NULL) {
        return XUD_INVALID_ARG;
    }
    if (context->ui == NULL) {
        return XUD_UNSUPPORTED;
    }
    return xud_ui_frame_draw(context->ui);
}
