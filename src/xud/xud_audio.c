/***********************************************************************************************
 *   xud_audio.c - the audio rung: miniaudio's decoder over one host PCM block, the record
 *                 behind an AUDIOSTREAM-kind slot of the handle layer, and the public load/
 *                 close pair and the two queries xud.h declares above it
 *
 *   Owns : the six rungs xud_audio.h declares (xud_audio_check(), xud_audio_result_from_ma(),
 *          xud_audio_bytes_of_frames(), xud_audio_seconds_from_frames(), xud_audio_frames_from_
 *          seconds(), xud_audio_get(), xud_audio_open(), xud_audio_close(), xud_audio_close_all
 *          ()) and the four public calls xud.h declares above them (xud_load_audio(), xud_close_
 *          audio(), xud_get_audio_info(), xud_get_audio_length()). The split is the point of the
 *          file: everything here is host-side, so the whole rung is checkable with no driver pre
 *          sent, and the only thing this file calls outside this tree is miniaudio - no Vulkan
 *          entry point and no driver function at all, which is the one fact that makes the audio
 *          area's checks headless rather than bake-shaped or parse-shaped like the font's and the
 *          model's are.
 *
 *   What the rung owns and what it borrows: it owns the record, the one PCM block miniaudio allo
 *          cates for it, and the rule about which of its two teardown paths frees them (xud_audio_
 *          close() at a slot release, xud_audio_close_all() at the table's teardown, exactly one of
 *          the two ever runs per record because close() clears the slot's field before it frees). It
 *          borrows the handle table (the slot, and the record field through the handle layer's own two
 *          accessors) and nothing else - not the loader, not the device, not the allocator, not the
 *          renderer - which is why this rung's open takes three arguments where the model's and the
 *          font's take six and seven. The block goes through miniaudio's own free rather than through
 *          libc's because it came from miniaudio's allocator, which is libc's today but is a callback
 *          pair this library is not passing.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud_audio.h" /* Required for: XudAudioStreamRecord and the rungs this file answers */
#include "xud.h" /* Required for: XudResult, XudHandle, XudAudioData, XudAudioInfo, XUD_HANDLE_KIND_AUDIOSTREAM - the public surface above */
#include "xud_core.h" /* Required for: struct XudContext, whose handleTable the public calls reach */
#include "xud_handle.h" /* Required for: XudHandleTable, xud_handle_validate/alloc/free, xud_handle_record_get/set - the slot this record lives in */
#include "xud_sound.h" /* Required for: xud_sound_detach - the one call the public close runs ABOVE this rung rather than beside it, for the reason the row's own note in that header states */

#include <assert.h> /* Required for: static_assert, the public token mirror below */
#include <stdlib.h> /* Required for: calloc, free - the record this rung spends */

/*-----------------------------------------------------------------------------------------------
 * The public token mirror, and why it is an assert rather than a translation
 *-----------------------------------------------------------------------------------------------*/

/* XUD_HANDLE_KIND_AUDIOSTREAM is xud.h's tag and the AUDIOSTREAM array this rung's records live in
   is the handle layer's, so the one thing this file can state is that the tag it opens a slot with is
   the tag a caller's handle carries back - the same mirror every other rung of this tree states, and
   the reason a font handle is refused here rather than freed from this kind's array.
*/
static_assert(XUD_HANDLE_KIND_AUDIOSTREAM > XUD_HANDLE_KIND_NONE,
    "xud_audio.c: the audio kind must be a kind value rather than the tag no live slot ever has, or the slot this rung opens is a slot the "
    "handle layer refuses");

/*-----------------------------------------------------------------------------------------------
 * The description's own check, and the two bounds it is stated against
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_audio_check(const XudAudioData* data) {
    if (data == NULL) {
        return XUD_INVALID_ARG;
    }
    if ((data->fileData == NULL) || (data->fileBytes < XUD_AUDIO_MIN_FILE_BYTES)) {
        /* No bytes and bytes too short to be a container at all are one answer: a description that
           names nothing to read and a description that names a fragment of one are both descriptions
           this rung cannot lay out, and both are the caller's fault rather than this decoder's.
        */
        return XUD_INVALID_ARG;
    }
    if (data->fileBytes > XUD_AUDIO_MAX_FILE_BYTES) {
        /* The container bound, and the direction it is stated in: this is the only input bound this
           rung can apply BEFORE miniaudio's own doubling allocation, so it is a bound on what this
           decoder will spend rather than on what this library will hold - the PCM ceiling below is
           the other half of the pair and refuses after the block exists.
        */
        return XUD_INVALID_ARG;
    }
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * miniaudio's answer as this library's code
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_audio_result_from_ma(ma_result result) {
    switch (result) {
    case MA_SUCCESS:
        return XUD_OK;
    case MA_OUT_OF_MEMORY:
        /* miniaudio's own allocator is libc's today, so its out-of-memory is this library's out-
           of-memory rather than a driver's refusal - the same reason the model rung gives for the
           cgltf code beside it.
        */
        return XUD_OUT_OF_MEMORY;
    case MA_INVALID_ARGS:
        /* A decoder-level bad argument is this library's own check's subject rather than the
           caller's: this rung hands the decoder a config it built itself, so a decoder that
           calls that bad is a rung that built the wrong config, which is this file's fault.
        */
        return XUD_INTERNAL_ERROR;
    case MA_TOO_BIG:
        /* miniaudio's own size bound, which is MA_SIZE_MAX rather than this rung's ceiling, and
           is the same shape as a set that fits no atlas this rung will try: the description is
           well formed and this rung's bound refused it.
        */
        return XUD_UNSUPPORTED;
    case MA_INVALID_FILE:
    case MA_INVALID_DATA:
    case MA_CRC_MISMATCH:
    case MA_FORMAT_NOT_SUPPORTED:
    case MA_NO_BACKEND:
    case MA_NO_DEVICE:
    case MA_BACKEND_NOT_ENABLED:
    case MA_INVALID_OPERATION:
    case MA_OUT_OF_RANGE:
    case MA_NOT_IMPLEMENTED:
    case MA_ERROR:
        /* One answer for a buffer that is not a container this decoder reads and for one it is
           but cannot walk: this library has no way to tell a caller that handed a buffer the
           difference, which is the reason the font rung gives for the same pair.
        */
        return XUD_UNSUPPORTED;
    default:
        return XUD_INTERNAL_ERROR;
    }
}

/*-----------------------------------------------------------------------------------------------
 * The stride arithmetic, and the two conversions between frames and seconds
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_audio_bytes_of_frames(size_t* outBytes, uint64_t frameCount, uint32_t channels) {
    uint64_t bytes = 0u;

    if (outBytes == NULL) {
        return XUD_INVALID_ARG;
    }
    *outBytes = 0u;
    if ((channels == 0u) || (channels > XUD_AUDIO_MAX_CHANNELS)) {
        /* A channel count of zero is a stream with no samples per frame, which is a stream this
           rung cannot own rather than an empty one, and a count past this rung's bound is a stream
           with no mixer path here - both are the same code the ceiling below answers.
        */
        return XUD_UNSUPPORTED;
    }
    if (frameCount > (uint64_t)(XUD_AUDIO_MAX_FRAME_BYTES / (XUD_AUDIO_BYTES_PER_SAMPLE * channels))) {
        /* The bound stated as a division rather than as a multiply so the multiply cannot overflow
           before it is compared - the same shape every other bound in this tree is stated in.
        */
        return XUD_UNSUPPORTED;
    }

    bytes = frameCount * (uint64_t)channels * (uint64_t)XUD_AUDIO_BYTES_PER_SAMPLE;
    *outBytes = (size_t)bytes;
    return XUD_OK;
}

XudResult xud_audio_seconds_from_frames(double* outSeconds, uint64_t frameCount, uint32_t sampleRate) {
    if (outSeconds == NULL) {
        return XUD_INVALID_ARG;
    }

    *outSeconds = 0.0;
    if (sampleRate == 0u) {
        /* A record that never had a rate is not a record this rung can divide against - the same
           answer xud_time_seconds_from_ticks() gives for a frequency of zero, and for the same re
           ason.
        */
        return XUD_UNSUPPORTED;
    }

    *outSeconds = (double)frameCount / (double)sampleRate;
    return XUD_OK;
}

XudResult xud_audio_frames_from_seconds(uint64_t* outFrames, double seconds, uint32_t sampleRate) {
    double frames = 0.0;

    if (outFrames == NULL) {
        return XUD_INVALID_ARG;
    }
    *outFrames = 0u;
    if (sampleRate == 0u) {
        return XUD_UNSUPPORTED;
    }
    if (seconds < 0.0) {
        /* A negative span is refused rather than wrapped: a sound placed at a negative time is a
           sound whose frames have no row to start at, and wrapping turns that into a sound placed
           at the end of every stream.
        */
        return XUD_UNSUPPORTED;
    }

    frames = seconds * (double)sampleRate;
    if (frames >= (double)XUD_AUDIO_MAX_FRAMES) {
        /* The ceiling is compared against the double rather than against the uint64_t after the
           cast, because a cast of an out-of-range double is undefined rather than a large number -
           the comparison has to happen in the type the value is actually in.
        */
        return XUD_UNSUPPORTED;
    }

    *outFrames = (uint64_t)frames;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The accessor
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_audio_get(const XudHandleTable* table, XudHandle handle, const XudAudioStreamRecord** outRecord) {
    void* record = NULL;

    if ((outRecord == NULL) || (table == NULL)) {
        return XUD_INVALID_ARG;
    }
    *outRecord = NULL;
    if (xud_handle_validate(table, handle, XUD_HANDLE_KIND_AUDIOSTREAM) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    if (xud_handle_record_get(table, XUD_HANDLE_KIND_AUDIOSTREAM, handle.index, &record) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    if (record == NULL) {
        return XUD_INVALID_ARG;
    }

    *outRecord = (const XudAudioStreamRecord*)record;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The driver half: one decode, one block, one slot
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_audio_open(XudHandle* outHandle, XudHandleTable* table, const XudAudioData* data) {
    XudAudioStreamRecord* record = NULL;
    XudHandle streamHandle;
    ma_decoder_config config;
    ma_uint64 frameCount = 0u;
    size_t frameBytes = 0u;
    XudResult result = XUD_INTERNAL_ERROR;

    if ((outHandle == NULL) || (table == NULL) || (data == NULL)) {
        return XUD_INVALID_ARG;
    }

    /* The description's check runs before a slot is spent, for the rule every rung in this tree
       states: a refusal leaves no half-stream behind. This is also the one place the container bound
       is applied, which is the only bound this rung can apply before miniaudio's own allocation.
    */
    result = xud_audio_check(data);
    if (result != XUD_OK) {
        return result;
    }

    result = xud_handle_alloc(outHandle, table, XUD_HANDLE_KIND_AUDIOSTREAM);
    if (result != XUD_OK) {
        return result;
    }
    streamHandle = *outHandle;

    record = (XudAudioStreamRecord*)calloc(1u, sizeof(XudAudioStreamRecord));
    if (record == NULL) {
        (void)xud_handle_free(table, streamHandle);
        *outHandle = (XudHandle) { 0u, 0u, XUD_HANDLE_KIND_NONE };
        return XUD_OUT_OF_MEMORY;
    }

    /* The one decode, and the one-shot helper rather than a decoder this rung drives: miniaudio's
       helper allocates the block itself, doubling from 4096 frames until the read ends, and fills
       the config it was handed with what it actually got rather than with what it was asked for.
       The rate and the channel count are therefore read out of the config below rather than out of
       the description, which is the reason the record's fields are named "what miniaudio answered".
    */
    config = ma_decoder_config_init(ma_format_f32, 0u, 0u);
    result = xud_audio_result_from_ma(
        ma_decode_memory(data->fileData, (size_t)data->fileBytes, &config, &frameCount, (void**)&record->frames));
    if (result != XUD_OK) {
        free(record);
        (void)xud_handle_free(table, streamHandle);
        *outHandle = (XudHandle) { 0u, 0u, XUD_HANDLE_KIND_NONE };
        return result;
    }

    if (config.format != ma_format_f32) {
        /* This rung asked for f32 and states what it got rather than assuming it: a decoder that
           answered another format is a decoder this library has no mixer path for, and the block
           it wrote is in that other format, so the refusal is a refusal rather than a stream whose
           samples would be read at the wrong stride.
        */
        ma_free(record->frames, NULL);
        free(record);
        (void)xud_handle_free(table, streamHandle);
        *outHandle = (XudHandle) { 0u, 0u, XUD_HANDLE_KIND_NONE };
        return XUD_UNSUPPORTED;
    }

    record->channels = config.channels;
    record->sampleRate = config.sampleRate;
    record->frameCount = frameCount;

    /* The PCM half of the bound pair, and the direction it runs in: the block already exists by the
       time this comparison can be made, because the frame count is unknown until the read ends, so
       this is a ceiling on a stream this library will HOLD rather than a ceiling on a stream this
       decoder will spend. The block goes back through miniaudio's own free for the reason the record's
       type note gives.
    */
    if ((record->channels > XUD_AUDIO_MAX_CHANNELS) || (record->sampleRate == 0u) || (record->sampleRate > XUD_AUDIO_MAX_SAMPLE_RATE)) {
        ma_free(record->frames, NULL);
        free(record);
        (void)xud_handle_free(table, streamHandle);
        *outHandle = (XudHandle) { 0u, 0u, XUD_HANDLE_KIND_NONE };
        return XUD_UNSUPPORTED;
    }
    if (xud_audio_bytes_of_frames(&frameBytes, record->frameCount, record->channels) != XUD_OK) {
        /* The PCM half of the bound pair, run through the stride rung rather than by a second copy of
           its arithmetic: the block's size is miniaudio's own doubling sequence and this rung only has
           to know that the frame count it is about to record is one whose stride it will hold, which is
           the same question the rung answers for its own out-bytes.
        */
        ma_free(record->frames, NULL);
        free(record);
        (void)xud_handle_free(table, streamHandle);
        *outHandle = (XudHandle) { 0u, 0u, XUD_HANDLE_KIND_NONE };
        return XUD_UNSUPPORTED;
    }

    if (xud_handle_record_set(table, XUD_HANDLE_KIND_AUDIOSTREAM, streamHandle.index, record) != XUD_OK) {
        ma_free(record->frames, NULL);
        free(record);
        (void)xud_handle_free(table, streamHandle);
        *outHandle = (XudHandle) { 0u, 0u, XUD_HANDLE_KIND_NONE };
        return XUD_INVALID_ARG;
    }

    return XUD_OK;
}

XudResult xud_audio_close(XudHandleTable* table, XudHandle handle) {
    XudAudioStreamRecord* record = NULL;
    void* found = NULL;

    if (table == NULL) {
        return XUD_INVALID_ARG;
    }

    /* Validated against the AUDIOSTREAM kind rather than against the handle's own tag: releasing is
       only ever about the array this rung's records live in, so a handle of another kind is refused
       rather than freeing a record of that other kind - the same test every other close in this tree
       runs, and the same reason.
    */
    if (xud_handle_validate(table, handle, XUD_HANDLE_KIND_AUDIOSTREAM) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    if (xud_handle_record_get(table, XUD_HANDLE_KIND_AUDIOSTREAM, handle.index, &found) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    record = (XudAudioStreamRecord*)found;

    /* Cleared BEFORE either free, and before xud_handle_free() marks the slot free: the handle layer's
       own note says a released slot "leaves the record as it was", so a free slot can otherwise still
       own a heap record and two teardown paths would both free it. The block goes before the record
       because the record names the block, and the block goes through miniaudio's own free because it
       came from miniaudio's allocator.
    */
    if (record != NULL) {
        if (xud_handle_record_set(table, XUD_HANDLE_KIND_AUDIOSTREAM, handle.index, NULL) != XUD_OK) {
            return XUD_INVALID_ARG;
        }
        ma_free(record->frames, NULL);
        free(record);
    }

    return xud_handle_free(table, handle);
}

XudResult xud_audio_close_all(XudHandleTable* table, uint32_t* outClosed) {
    void* found = NULL;
    XudAudioStreamRecord* record = NULL;
    uint32_t index = 0u;

    if ((table == NULL) || (outClosed == NULL)) {
        return XUD_INVALID_ARG;
    }
    *outClosed = 0u;
    if (table->slots[XUD_HANDLE_KIND_AUDIOSTREAM] == NULL) {
        /* No AUDIOSTREAM array was ever allocated: nothing of this kind was ever created, which is
           a count of zero and not an error - the same answer xud_handle_table_live_count() gives for
           the same reason.
        */
        return XUD_OK;
    }

    /* Every row of the array is walked, live or free: xud_handle_free() leaves the record field as it
       was, so a slot a caller released still owns its record until this rung clears it, and a record
       left behind is a leak the table's own close cannot see - it frees the array, not what the array
       points at, and the block behind the record is a second allocation that close cannot see either.
       The walk is bounded by the array's own high mark rather than by its capacity, so the rows past
       the last slot ever handed out are not read.
    */
    for (index = 0u; index < table->slotCount[XUD_HANDLE_KIND_AUDIOSTREAM]; index++) {
        if (xud_handle_record_get(table, XUD_HANDLE_KIND_AUDIOSTREAM, index, &found) != XUD_OK) {
            return XUD_INVALID_ARG;
        }
        record = (XudAudioStreamRecord*)found;
        if (record != NULL) {
            if (xud_handle_record_set(table, XUD_HANDLE_KIND_AUDIOSTREAM, index, NULL) != XUD_OK) {
                return XUD_INVALID_ARG;
            }
            ma_free(record->frames, NULL);
            free(record);
            (*outClosed)++;
        }
    }

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The public surface above the rung
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_load_audio(XudHandle* outHandle, XudContext* context, const XudAudioData* audio) {
    if ((outHandle == NULL) || (context == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (context->handleTable == NULL) {
        /* A context with no handle table is a context that never completed bring-up: xud_init_
           context() hands back NULL for that case, so this is a caller holding on rather than a
           state the library leaves behind - the same answer xud_begin_drawing() and xud_create_
           material() give for the same reason.
        */
        return XUD_UNSUPPORTED;
    }

    return xud_audio_open(outHandle, context->handleTable, audio);
}

XudResult xud_close_audio(XudContext* context, XudHandle handle) {
    if (context == NULL) {
        return XUD_INVALID_ARG;
    }
    if (context->handleTable == NULL) {
        return XUD_UNSUPPORTED;
    }

    /* The sound rung's row given back before this rung frees the record it reads: a row is a ma_au
       dio_buffer_ref over the block below and a node attached to a graph that is reading it, so a
       close that went first would be a mixer reading memory that was just freed. The rung above is
       asked first and it refuses (XUD_UNSUPPORTED) while the row is playing, which is the honest a
       nswer rather than a stop this rung would have to do on the caller's behalf.
    */
    if (context->sound != NULL) {
        XudResult rung = xud_sound_detach(context->sound, context->handleTable, handle);
        if (rung != XUD_OK) {
            return rung;
        }
    }

    return xud_audio_close(context->handleTable, handle);
}

XudResult xud_get_audio_info(XudAudioInfo* outInfo, XudContext* context, XudHandle handle) {
    const XudAudioStreamRecord* record = NULL;
    XudResult result = XUD_INTERNAL_ERROR;

    if ((outInfo == NULL) || (context == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (context->handleTable == NULL) {
        return XUD_UNSUPPORTED;
    }

    result = xud_audio_get(context->handleTable, handle, &record);
    if (result != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    /* The record's own three numbers, copied rather than handed out: the record is the library's and
       is about to be freed at the next close, so what a caller can hold is a value rather than a
       pointer into a slot.
    */
    outInfo->channels = record->channels;
    outInfo->sampleRate = record->sampleRate;
    outInfo->frameCount = record->frameCount;
    return XUD_OK;
}

XudResult xud_get_audio_length(float* outSeconds, XudContext* context, XudHandle handle) {
    const XudAudioStreamRecord* record = NULL;
    double seconds = 0.0;
    XudResult result = XUD_INTERNAL_ERROR;

    if ((outSeconds == NULL) || (context == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (context->handleTable == NULL) {
        return XUD_UNSUPPORTED;
    }

    result = xud_audio_get(context->handleTable, handle, &record);
    if (result != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    /* The divide is the pure rung's and the float is raylib's: GetAudioLength answers a float, and
       a stream longer than a float of seconds can name is a stream this library's own PCM ceiling
       refuses long before the conversion loses anything - the ceiling is 64 MiB of PCM, which is
       under 8 million seconds at any rate this rung admits.
    */
    result = xud_audio_seconds_from_frames(&seconds, record->frameCount, record->sampleRate);
    if (result != XUD_OK) {
        return result;
    }

    *outSeconds = (float)seconds;
    return XUD_OK;
}
