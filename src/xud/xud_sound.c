/***********************************************************************************************
 *   xud_sound.c - the sound rung: miniaudio's engine above the audio rung's one host block, the
 *                 one node pair behind one AUDIOSTREAM slot, and the play/stop pair and the one
 *                 question xud.h declares above it
 *
 *   Owns : the seven rungs xud_sound.h declares (xud_sound_result_from_ma(), xud_sound_open(),
 *          xud_sound_play(), xud_sound_stop(), xud_sound_is_playing(), xud_sound_detach(), xud_
 *          sound_close()) and the three public calls xud.h declares above them (xud_play_audio(),
 *          xud_stop_audio(), xud_is_audio_playing()). The split is the point of the file: the
 *          audio rung below owns the frames and owns nothing that moves, and this rung owns the
 *          one thing that does move - a node attached to a graph, a device with a thread behind
 *          it and a mixer above both.
 *
 *   What the rung owns and what it borrows: it owns the engine (and with it the node graph, the
 *          ma_device inside it and the mixer), the row array, the rows and each row's node pair.
 *          It borrows the audio record's block through the audio rung's own accessor and never a
 *          frees it - the bufferRef a row holds is a reference OVER the record's block, so the b
 *          lock has exactly one owner, the audio record, and this rung's teardown gives the acce
 *          ss back rather than the memory. It borrows the handle table for the slot index that in
 *          dexes the row array and nothing else - not the loader, not the device layer, not the
 *          allocator, not the renderer - which is why this rung's open takes one argument where
 *          the renderer's takes seven.
 *
 *   Why the row array holds pointers rather than rows: miniaudio's own node attach puts a row's
 *   sound struct INTO the graph as an item of the endpoint's input-bus linked list, so the graph
 *   holds an address inside the row. A row array that grew by moving rows would leave the endpoi
 *   nt naming the address a row moved from, which is a mixer reading a row that is no longer the
 *   row the graph was told about. The array of pointers grows by moving pointers, which is what
 *   keeps the graph's address valid, and it is the reason the row type's own note in the header
 *   states the same split rather than restating it here.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud_sound.h" /* Required for: XudSound, XudSoundRow, XUD_SOUND_ROW_STATE_* and the rungs this file answers */
#include "xud.h" /* Required for: XudResult, XudHandle, XUD_HANDLE_KIND_AUDIOSTREAM - the public surface above */
#include "xud_audio.h" /* Required for: XudAudioStreamRecord and xud_audio_get() - the frames this rung's nodes read */
#include "xud_core.h" /* Required for: struct XudContext, whose sound field the public calls reach */
#include "xud_handle.h" /* Required for: XudHandleTable and the slot index that indexes the row array */

#include <assert.h> /* Required for: static_assert, the public token mirror below */
#include <stdlib.h> /* Required for: calloc, free - the rung, the rows and the row array */
#include <string.h> /* Required for: memcpy, the one move the row array's own growth makes */

/*-----------------------------------------------------------------------------------------------
 * The public token mirror, and why it is an assert rather than a translation
 *-----------------------------------------------------------------------------------------------*/

/* The row array is indexed by an AUDIOSTREAM slot index, so the one thing this file can state is
   that the index it indexes with is an index of the kind the handle it was handed carries - the
   same mirror every other rung of this tree states, and the reason a MATERIAL handle whose index
   happens to be small is refused here rather than read as a row of a stream that was never loade
   d.
*/
static_assert(XUD_HANDLE_KIND_AUDIOSTREAM > XUD_HANDLE_KIND_NONE,
    "xud_sound.c: the audio kind must be a kind value rather than the tag no live slot ever has, or the row this rung indexes is a row the "
    "handle layer refuses");

/*-----------------------------------------------------------------------------------------------
 * miniaudio's answer as this library's code, and the one answer this rung adds to the audio
 * rung's table
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_sound_result_from_ma(ma_result result) {
    switch (result) {
    case MA_SUCCESS:
        return XUD_OK;
    case MA_OUT_OF_MEMORY:
        /* miniaudio's own allocator is libc's today, so its out-of-memory is this library's out-
           of-memory rather than a driver's refusal - the same reason the audio rung gives beside
           it.
        */
        return XUD_OUT_OF_MEMORY;
    case MA_NO_BACKEND:
    case MA_NO_DEVICE:
    case MA_BACKEND_NOT_ENABLED:
        /* The one shape this rung can hit that the audio rung cannot: a decoder has no backend to
           ask for and a device does. This is the honest answer rather than a half-open rung, and
           it is the reason the play call's own note states a refusal that is not the caller's fau
           lt.
        */
        return XUD_UNSUPPORTED;
    case MA_INVALID_ARGS:
    case MA_INVALID_OPERATION:
    case MA_OUT_OF_RANGE:
    case MA_TOO_BIG:
    case MA_NOT_IMPLEMENTED:
    case MA_ERROR:
        /* This rung hands miniaudio a config it built itself, so a bad argument or a bad operati
           on below this call is this file's shape rather than a caller's - the same reading the
           audio rung gives for a decoder that calls a config bad.
        */
        return XUD_INTERNAL_ERROR;
    default:
        return XUD_INTERNAL_ERROR;
    }
}

/*-----------------------------------------------------------------------------------------------
 * The row array's own growth, and the one reason it holds pointers
 *-----------------------------------------------------------------------------------------------*/

static XudResult xud_sound_rows_grow(XudSound* sound, uint32_t slotIndex) {
    XudSoundRow** grown = NULL;
    uint32_t wanted = XUD_SOUND_ROW_MIN_CAPACITY;
    uint32_t index = 0u;

    if ((sound == NULL) || (slotIndex >= XUD_SOUND_ROW_MAX_CAPACITY)) {
        /* The ceiling is the handle layer's slot ceiling, so an index past it is an index the ta
           ble could not have handed out - this rung's own bound rather than a caller's argument.
        */
        return XUD_INTERNAL_ERROR;
    }

    while (wanted <= slotIndex) {
        wanted
            *= 2u; /* the doubling sequence the handle layer's own arrays use, landing on the ceiling exactly because of the assert above */
    }

    if ((sound->rows != NULL) && (sound->rowCapacity >= wanted)) {
        return XUD_OK;
    }

    grown = (XudSoundRow**)calloc((size_t)wanted, sizeof(XudSoundRow*));
    if (grown == NULL) {
        return XUD_OUT_OF_MEMORY;
    }
    if (sound->rows != NULL) {
        memcpy(grown, sound->rows, (size_t)sound->rowCount * sizeof(XudSoundRow*));
    }
    for (index = sound->rowCount; index < wanted; index++) {
        grown[index]
            = NULL; /* the rows of the new capacity that no slot index has reached yet, which is what a row lookup below reads as "no node for this slot" */
    }

    free(sound->rows); /* the array of pointers, never what the pointers point at: a row that moved is a row the graph no longer names */
    sound->rows = grown;
    sound->rowCount
        = wanted; /* the high mark moves with the capacity, which is what bounds the teardown walk over rows that were never reached */
    sound->rowCapacity = wanted;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The bring-up half: the engine, the device inside it and the mixer above both
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_sound_open(XudSound** outSound) {
    XudSound* sound = NULL;
    ma_engine_config engineConfig;
    ma_device* device = NULL;
    ma_result maResult = MA_INVALID_ARGS;

    if ((outSound == NULL) || (*outSound != NULL)) {
        return XUD_INVALID_ARG;
    }

    sound = (XudSound*)calloc(1, sizeof(XudSound));
    if (sound == NULL) {
        return XUD_OUT_OF_MEMORY;
    }

    engineConfig = ma_engine_config_init();

    /* The engine, in miniaudio's own order and with miniaudio's own defaults: the node graph firs
       t, the ma_device under it (allocated by the engine, with ma_engine_data_callback_internal a
       s its data callback, which is the half that advances the graph while the device's thread pu
       lls frames), the mixer above both and the listeners beside them. No context is handed in an
       d no device is handed in, which is what makes the engine own both - the audio rung's note a
       bout the low-level shape states the other half of that split, that a bare device and graph
       would leave the loop to whoever owns them.
    */
    maResult = ma_engine_init(&engineConfig, &sound->engine);
    if (maResult != MA_SUCCESS) {
        free(sound);
        return xud_sound_result_from_ma(maResult);
    }

    device = ma_engine_get_device(&sound->engine);
    if (device == NULL) {
        /* The engine's own note says the device is optional only when a config says so, and this
           rung's config does not say so, so an engine with no device inside it is this file's sh
           ape rather than a machine's refusal.
        */
        ma_engine_uninit(&sound->engine);
        free(sound);
        return XUD_INTERNAL_ERROR;
    }

    /* The device, which miniaudio opens asleep: every device of every backend this vendored minia
       udio drives needs an explicit start, which is the one line the engine's own bring-up does
       not do for it.
    */
    maResult = ma_device_start(device);
    if (maResult != MA_SUCCESS) {
        ma_engine_uninit(&sound->engine);
        free(sound);
        return xud_sound_result_from_ma(maResult);
    }

    *outSound = sound;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The play half, and the one node behind one slot
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_sound_play(XudSound* sound, XudHandleTable* table, XudHandle stream) {
    const XudAudioStreamRecord* record = NULL;
    XudSoundRow* row = NULL;
    ma_sound_config soundConfig;
    ma_result maResult = MA_INVALID_ARGS;
    XudResult result = XUD_INTERNAL_ERROR;

    if ((sound == NULL) || (table == NULL)) {
        return XUD_INVALID_ARG;
    }

    result = xud_audio_get(table, stream, &record);
    if (result != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    if (record->frameCount == 0ULL) {
        /* A stream of no frames is a stream the decoder walked and this rung's ceiling admitted,
           and it is a stream the mixer has no walk to read: the answer is a refusal rather than a
           node attached to an empty block, because a node that plays nothing is a node a caller
           cannot tell apart from a node that was never spent.
        */
        return XUD_UNSUPPORTED;
    }

    result = xud_sound_rows_grow(sound, stream.index);
    if (result != XUD_OK) {
        return result;
    }

    row = sound->rows[stream.index];
    if (row == NULL) {
        row = (XudSoundRow*)calloc(1, sizeof(XudSoundRow));
        if (row == NULL) {
            return XUD_OUT_OF_MEMORY;
        }

        /* The reference, over the record's block rather than over a copy of it: the audio record
           owns the block and this row names it, which is what keeps the block to one owner and th
           e audio rung's one PCM ceiling to one statement.
        */
        maResult
            = ma_audio_buffer_ref_init(ma_format_f32, record->channels, record->frames, (ma_uint64)record->frameCount, &row->bufferRef);
        if (maResult != MA_SUCCESS) {
            free(row);
            return xud_sound_result_from_ma(maResult);
        }

        soundConfig = ma_sound_config_init_2(&sound->engine);
        soundConfig.pDataSource = &row->bufferRef;
        soundConfig.flags = MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_PITCH;

        /* The node, attached to the engine's endpoint by the config's own default rather than by a
           bus this rung picked: one node per stream is this rung's decision and the endpoint is m
           iniaudio's, so the rung that decides the shape does not also decide the wiring.
        */
        maResult = ma_sound_init_ex(&sound->engine, &soundConfig, &row->sound);
        if (maResult != MA_SUCCESS) {
            ma_audio_buffer_ref_uninit(&row->bufferRef);
            free(row);
            return xud_sound_result_from_ma(maResult);
        }

        sound->rows[stream.index] = row;
        sound->liveRows++;
    }

    if (row->state == XUD_SOUND_ROW_STATE_PLAYING) {
        return XUD_ALREADY;
    }

    maResult = ma_sound_start(&row->sound);
    if (maResult != MA_SUCCESS) {
        return xud_sound_result_from_ma(maResult);
    }
    row->state = XUD_SOUND_ROW_STATE_PLAYING;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The stop half, the one question and the release
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_sound_stop(XudSound* sound, XudHandleTable* table, XudHandle stream) {
    const XudAudioStreamRecord* record = NULL;
    XudSoundRow* row = NULL;
    XudResult result = XUD_INTERNAL_ERROR;

    if ((sound == NULL) || (table == NULL)) {
        return XUD_INVALID_ARG;
    }

    result = xud_audio_get(table, stream, &record);
    if (result != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    if ((sound->rows == NULL) || (stream.index >= sound->rowCount)) {
        return XUD_ALREADY;
    }
    row = sound->rows[stream.index];
    if ((row == NULL) || (row->state != XUD_SOUND_ROW_STATE_PLAYING)) {
        /* A slot that never had a node and a slot whose node is already stopped are one answer an
           d it is the state this call asks for already being the state it has - the repo's own id
           empotent-teardown code, and the reason a stop run twice is not a stop that refused the
           second time.
        */
        return XUD_ALREADY;
    }

    ma_sound_stop(&row->sound);
    row->state = XUD_SOUND_ROW_STATE_STOPPED;

    return XUD_OK;
}

XudResult xud_sound_is_playing(bool* outPlaying, const XudSound* sound, XudHandleTable* table, XudHandle stream) {
    const XudAudioStreamRecord* record = NULL;
    const XudSoundRow* row = NULL;
    XudResult result = XUD_INTERNAL_ERROR;

    if ((outPlaying == NULL) || (sound == NULL) || (table == NULL)) {
        return XUD_INVALID_ARG;
    }

    result = xud_audio_get(table, stream, &record);
    if (result != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    *outPlaying = false;
    if ((sound->rows == NULL) || (stream.index >= sound->rowCount)) {
        return XUD_OK;
    }
    row = sound->rows[stream.index];
    if (row != NULL) {
        *outPlaying = (row->state == XUD_SOUND_ROW_STATE_PLAYING);
    }

    return XUD_OK;
}

XudResult xud_sound_detach(XudSound* sound, XudHandleTable* table, XudHandle stream) {
    const XudAudioStreamRecord* record = NULL;
    XudSoundRow* row = NULL;
    XudResult result = XUD_INTERNAL_ERROR;

    if ((sound == NULL) || (table == NULL)) {
        return XUD_INVALID_ARG;
    }

    result = xud_audio_get(table, stream, &record);
    if (result != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    if ((sound->rows == NULL) || (stream.index >= sound->rowCount)) {
        return XUD_OK;
    }
    row = sound->rows[stream.index];
    if (row == NULL) {
        return XUD_OK;
    }
    if (row->state == XUD_SOUND_ROW_STATE_PLAYING) {
        /* The refusal the audio close runs this half for: the row's node is reading the block the
           record is about to free, and a close that stopped the row on the caller's behalf would
           be a close that changed what a caller hears without being asked to.
        */
        return XUD_UNSUPPORTED;
    }

    /* The node first and the reference second, for the reason the node's own uninit states: it de
       taches from the graph and does not return until it has, so the mixer is no longer reading t
       he row by the time the reference goes, and the reference is given back rather than the memo
       ry because the audio record owns the block below.
    */
    ma_sound_uninit(&row->sound);
    ma_audio_buffer_ref_uninit(&row->bufferRef);
    free(row);
    sound->rows[stream.index] = NULL;
    sound->liveRows--;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The teardown rung, above the audio rung's own close_all
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_sound_close(XudSound** sound, uint32_t* outClosed) {
    XudSound* victim = NULL;
    XudSoundRow* row = NULL;
    uint32_t index = 0u;

    if ((sound == NULL) || (outClosed == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (*sound == NULL) {
        /* A rung that was never opened is the state a context that never played anything leaves
           behind, and closing it is the state it asks for already being the state it has - the s
           ame answer every other close in this tree gives for the same reason.
        */
        return XUD_ALREADY;
    }

    victim = *sound;
    *outClosed = 0u;

    /* Every row of the array, live or not: a row is a node attached to a graph that is about to b
       e gone, and a node attached to a graph that is gone is a graph holding an address inside a
       row that is about to be freed. The walk is bounded by the array's own high mark rather than
       by its capacity, which is the same reason every other walk in this tree gives.
    */
    for (index = 0u; index < victim->rowCount; index++) {
        row = victim->rows[index];
        if (row != NULL) {
            ma_sound_uninit(&row->sound);
            ma_audio_buffer_ref_uninit(&row->bufferRef);
            free(row);
            victim->rows[index] = NULL;
            (*outClosed)++;
        }
    }
    victim->liveRows = 0u;

    /* The engine last of the three, because it is the graph the rows were attached to and the dev
       ice whose thread was pulling from it: the rows went first, so nothing is attached when the
       graph goes, and the device's own uninit stops it before it is destroyed.
    */
    ma_engine_uninit(&victim->engine);

    free(victim->rows);
    free(victim);
    *sound = NULL;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The public surface above the rung
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_play_audio(XudContext* context, XudHandle stream) {
    const XudAudioStreamRecord* record = NULL;
    XudResult result = XUD_INTERNAL_ERROR;

    if (context == NULL) {
        return XUD_INVALID_ARG;
    }
    if (context->handleTable == NULL) {
        return XUD_UNSUPPORTED;
    }

    /* The handle validated BEFORE the rung is spent, which is the reason the play call's own note
       states a refusal that is not the caller's fault rather than a bad argument: a stale or a wro
       ng-kind handle must refuse as a bad argument even on a machine with no audio backend, and a
       rung spent first would answer the backend's absence instead - the same order the stop and th
       e question below give, and the reason the audio area's checks for the pair stay headless.
    */
    result = xud_audio_get(context->handleTable, stream, &record);
    if (result != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    if (context->sound == NULL) {
        result = xud_sound_open(&context->sound);
        if (result != XUD_OK) {
            return result;
        }
    }

    return xud_sound_play(context->sound, context->handleTable, stream);
}

XudResult xud_stop_audio(XudContext* context, XudHandle stream) {
    const XudAudioStreamRecord* record = NULL;
    XudResult result = XUD_INTERNAL_ERROR;

    if (context == NULL) {
        return XUD_INVALID_ARG;
    }
    if (context->handleTable == NULL) {
        return XUD_UNSUPPORTED;
    }

    /* The handle validated even on a context that never opened the rung, so a stop handed a stale
       handle refuses as a bad argument rather than as "already stopped", which is the same reaso
       n the audio rung's own accessor gives for the same handle.
    */
    result = xud_audio_get(context->handleTable, stream, &record);
    if (result != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    if (context->sound == NULL) {
        return XUD_ALREADY;
    }

    return xud_sound_stop(context->sound, context->handleTable, stream);
}

XudResult xud_is_audio_playing(bool* outPlaying, XudContext* context, XudHandle stream) {
    const XudAudioStreamRecord* record = NULL;
    XudResult result = XUD_INTERNAL_ERROR;

    if ((outPlaying == NULL) || (context == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (context->handleTable == NULL) {
        return XUD_UNSUPPORTED;
    }

    result = xud_audio_get(context->handleTable, stream, &record);
    if (result != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    if (context->sound == NULL) {
        *outPlaying = false;
        return XUD_OK;
    }

    return xud_sound_is_playing(outPlaying, context->sound, context->handleTable, stream);
}
