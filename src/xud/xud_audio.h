/***********************************************************************************************
 *
 *   xud_audio.h - the audio rung: miniaudio's decoder over one host PCM block, the stream
 *                 record behind an AUDIOSTREAM-kind slot, and the load/close pair xud.h
 *                 declares on top of it
 *
 *   Owns : XudAudioStreamRecord - the record behind an AUDIOSTREAM-kind slot of the handle
 *          layer - plus the pure halves above it (xud_audio_check(), xud_audio_result_from_ma
 *          (), xud_audio_bytes_of_frames(), xud_audio_seconds_from_frames(), xud_audio_frames_
 *          from_seconds()), the accessor (xud_audio_get()), the driver halves (xud_audio_open
 *          (), xud_audio_close()) and the teardown rung above the table's own close (xud_audio
 *          _close_all()).
 *   Layer: above the handle layer and below every rung that touches the driver. This is the
 *          FIRST rung whose driver half calls no Vulkan entry point and no driver function at
 *          all: a decode is miniaudio reading a caller's bytes into a heap block it allocates,
 *          so the whole rung is checkable with no device present, which is why the audio area's
 *          checks run headless while the model and the font areas needed a parse or a bake to be
 *          checkable at all.
 *
 *   Why the decode is one block per stream rather than a reader a caller drives: raylib's AudioS
 *   tream is a whole decoded stream (audio.c: LoadAudioStream decodes the file into one buffer and
 *   stores it on the AudioStream), and every play call indexes that buffer rather than reopening the
 *   file. miniaudio's own one-shot helper (ma_decode_memory) is that same shape, so this rung spends
 *   one block per stream and keeps it; the alternative - a decoder read through ma_decoder_read_pcm_
 *   frames on every play row - is a seek per row and a decoder that cannot be rewound without a re-
 *   init, which is why raylib's own note that a stream is "already decoded" is the shape it has.
 *
 *   Why the frames are f32 rather than the file's own sample type: miniaudio's decoder answers f32
 *   for every container it reads (wav, flac and mp3 alike), and the mixer above has to multiply a
 *   sample by a volume and an envelope, which is an f32 question. A second, int16 stream shape would
 *   be a second mixer path beside the one that exists, so this rung asks for f32 and states that it
 *   asked rather than assuming what it got - the record carries the rate and the channel count minia
 *   udio actually answered, not the ones the description asked for.
 *
 *   What this rung does NOT own, and why: no playback. raylib's PlayAudio/StopAudio/SetAudioFootpr
 *   int half is a device with a mixer and a queue of sounds above it, and miniaudio's own low-level
 *   device (ma_device) plays a graph of nodes rather than a buffer, so the play half is a second run
 *   g - the device and its graph - rather than a call above this one. What is landed is therefore eve
 *   rything a play row needs beside the device itself: the frames, the frame count, the channel coun
 *   t and the rate, plus the two conversions between frames and seconds a play row's time argument ne
 *   eds. Also absent for the reason AGENTS.md states rather than built here: the containers this ven
 *   dored miniaudio cannot read - ogg (stb_vorbis is not inlined in the vendored copy), qoa and xm/mo
 *   d - answer MA_INVALID_FILE rather than a half-decoded stream, and adding those decoders is an as
 *   k to include their headers, not a decoder to write here.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#ifndef XUD_XUD_AUDIO_H_
#define XUD_XUD_AUDIO_H_

#include "miniaudio.h" /* Required for: ma_decoder_config, ma_decoder, ma_result, ma_format_f32 - the decoder this rung runs. Included again here on purpose: the implementation lives in xud_vendor_miniaudio.c, and this header is the caller's half of that split, the same way xud_model.h includes cgltf.h and xud_font.h includes stb_truetype.h. */
#include "xud.h" /* Required for: XudResult, XudHandle, XudAudioData, XUD_HANDLE_KIND_AUDIOSTREAM - the public surface this rung is the half of */
#include "xud_handle.h" /* Required for: XudHandleTable, xud_handle_validate/alloc/free, xud_handle_record_get/set - the slot this record lives in */

#include <assert.h> /* Required for: static_assert, the bounds below against each other */
#include <stddef.h> /* Required for: size_t - the one block size the frame arithmetic answers */
#include <stdint.h> /* Required for: uint32_t - the channel count, the rate and every count below */
#include <stdlib.h> /* Required for: malloc, free - the record this rung spends */

/*-----------------------------------------------------------------------------------------------
 * The rung's own bounds
 *-----------------------------------------------------------------------------------------------

   The two bounds are a pair, and the pair is stated in the two directions it has to be: the
   container bound is what keeps miniaudio's OWN doubling allocation bounded (it grows by doubling
   from 4096 frames until the read ends, so a caller's bytes are the only input this rung can bound
   BEFORE the allocation happens), and the PCM bound is the refusal that runs after it, on the frame
   count miniaudio answered. The PCM bound is therefore a ceiling on a stream this library will hold
   rather than a ceiling on a stream this library will spend, and the note says so rather than
   pretending the second bound comes first.
*/

#define XUD_AUDIO_MIN_FILE_BYTES                                                                                                           \
    12u /* the shortest header this rung's decoder sniffs: RIFF, a size and 'WAVE' is twelve bytes, and dr_wav's own sniff reads no less   \
           than that */
#define XUD_AUDIO_MAX_FILE_BYTES                                                                                                           \
    (16u * 1024u * 1024u) /* the largest container buffer this rung will read, which is what bounds miniaudio's own growth */
#define XUD_AUDIO_MAX_FRAME_BYTES                                                                                                          \
    (64u * 1024u                                                                                                                           \
        * 1024u) /* the largest PCM block this rung will hold, in bytes rather than in frames so the stride is visible beside it  \
                           */
#define XUD_AUDIO_BYTES_PER_SAMPLE 4u /* the one output format this rung asks for: ma_format_f32 is four bytes a sample */
#define XUD_AUDIO_MAX_CHANNELS                                                                                                             \
    8u /* the largest channel count this rung will hold: 7.1 is eight, and a stream with more has no mixer path here */
#define XUD_AUDIO_MAX_SAMPLE_RATE                                                                                                          \
    192000u /* the largest rate this rung will hold: 192 kHz is the highest rate any container it reads carries */

#define XUD_AUDIO_MAX_FRAMES                                                                                                               \
    (XUD_AUDIO_MAX_FRAME_BYTES                                                                                                             \
        / XUD_AUDIO_BYTES_PER_SAMPLE) /* the frame ceiling of a one-channel stream, which is the loosest of the     \
                                                                pair - the real ceiling is this divided by the channel count */

static_assert(XUD_AUDIO_MIN_FILE_BYTES < XUD_AUDIO_MAX_FILE_BYTES,
    "xud_audio.h: the shortest header must be shorter than the largest container, or the description's check has no range left in it");
static_assert(XUD_AUDIO_MAX_FRAME_BYTES >= XUD_AUDIO_MAX_FILE_BYTES,
    "xud_audio.h: a compressed container is smaller than the PCM it expands to, so the PCM bound has to be the looser of the two, or every "
    "lossless file refuses");
static_assert(XUD_AUDIO_MAX_FRAME_BYTES % XUD_AUDIO_BYTES_PER_SAMPLE == 0u,
    "xud_audio.h: the PCM bound must be a whole number of samples, or the frame ceiling below is a frame ceiling of a partial sample");
static_assert(XUD_AUDIO_MAX_FRAMES >= XUD_AUDIO_MAX_CHANNELS,
    "xud_audio.h: the frame ceiling must be at least a channel count, or the frame arithmetic below divides by a bound larger than its own "
    "ceiling");

/*-----------------------------------------------------------------------------------------------
 * The record behind an AUDIOSTREAM-kind slot
 *-----------------------------------------------------------------------------------------------

   One record per stream, and the record owns the one block miniaudio allocated for it. The
   block's CAPACITY is miniaudio's own doubling sequence and its LENGTH is frameCount, so nothing
   in this tree may size a copy or a walk from the buffer's own extent - every walk below is bounded
   by frameCount times channels, never by a sizeof of the block. The block is freed through miniaudio's
   own free rather than through libc's because the block came from miniaudio's allocator, which is
   libc's today but is a callback pair this library is not passing.
*/

typedef struct XudAudioStreamRecord {
    uint32_t channels; /* what miniaudio answered, not what this rung asked for */
    uint32_t sampleRate; /* what miniaudio answered, in Hz */
    uint64_t frameCount; /* one frame is one sample per channel, so the block is frameCount*channels samples */
    float* frames; /* interleaved PCM, miniaudio's own block, frameCount*channels floats of it */
} XudAudioStreamRecord;

/*-----------------------------------------------------------------------------------------------
 * The pure halves
 *-----------------------------------------------------------------------------------------------*/

/// Pure: the description's own check, run before a slot is spent so a refusal leaves no half-stream.
/// XUD_INVALID_ARG on a NULL description, on no bytes, on bytes shorter than the shortest header this
/// rung's decoder sniffs, or on bytes past this rung's container bound; XUD_OK otherwise.
XudResult xud_audio_check(const XudAudioData* data);

/// Pure: miniaudio's answer turned into this library's code. One row per the decoder's own code and no
/// fallthrough: MA_INVALID_FILE and MA_INVALID_DATA are one answer for a buffer that is not a container
/// this rung reads and for one it is but cannot walk - the same reason the font rung gives for not telling
/// a caller that handed a buffer the difference; MA_OUT_OF_MEMORY is this library's own code; MA_TOO_BIG is
/// this rung's ceiling rather than miniaudio's, so it is XUD_UNSUPPORTED like a set that fits no atlas. XUD_OK
/// on MA_SUCCESS; XUD_INTERNAL_ERROR on nothing - the default is that code so an unhandled shape still refuses.
XudResult xud_audio_result_from_ma(ma_result result);

/// Pure: the stride arithmetic a decode needs, overflow-guarded and bounded. XUD_INVALID_ARG on a NULL
/// out-bytes; XUD_UNSUPPORTED on a channel count of zero, past this rung's channel bound, or on a frame
/// count whose block this rung will not hold; XUD_OK on frameCount*channels*XUD_AUDIO_BYTES_PER_SAMPLE.
XudResult xud_audio_bytes_of_frames(size_t* outBytes, uint64_t frameCount, uint32_t channels);

/// Pure: the frame count as seconds - the clock rung's divide beside this rung's own counter, and the
/// number raylib's GetAudioLength answers. XUD_INVALID_ARG on a NULL out-seconds; XUD_UNSUPPORTED on a
/// rate of zero (a record that never had one is not a record this rung can divide against); XUD_OK otherwise.
XudResult xud_audio_seconds_from_frames(double* outSeconds, uint64_t frameCount, uint32_t sampleRate);

/// Pure: the inverse, which is what a play row's time argument needs before a device exists to answer it.
/// A negative span is refused rather than wrapped: a sound placed at a negative time is a sound whose
/// frames have no row to start at. Rounds toward zero, which is the same direction the divide above rounds
/// and is stated because a truncated row is a row that started a frame late rather than a row that started
/// a frame early. XUD_INVALID_ARG on a NULL out-frames; XUD_UNSUPPORTED on a rate of zero, on a negative
/// span, or on a span this rung's own frame ceiling cannot name; XUD_OK otherwise.
XudResult xud_audio_frames_from_seconds(uint64_t* outFrames, double seconds, uint32_t sampleRate);

/// Pure: the accessor the rung above reaches instead of the slot array - validate against the AUDIOSTREAM
/// kind, then hand back the record of a live slot. XUD_INVALID_ARG on a NULL out-record or table, or on a
/// handle that does not name a live AUDIOSTREAM slot (a stale handle, a cross-kind handle, a fabricated one),
/// or on a live slot whose record field is still the sentinel.
XudResult xud_audio_get(const XudHandleTable* table, XudHandle handle, const XudAudioStreamRecord** outRecord);

/*-----------------------------------------------------------------------------------------------
 * The driver halves
 *-----------------------------------------------------------------------------------------------*/

/// Driver: the whole rung in one call, in the one order that is correct. Order: the description's check
/// (before a slot is spent, so a refusal leaves no half-stream), the slot, the record, then miniaudio's
/// one-shot decode into a block it allocated, then the rate and the channel count read back out of the
/// config it filled rather than out of the description it was handed, then the frame count checked against
/// this rung's own ceiling, then the record written into the slot through the handle layer's own accessor.
/// This half calls no Vulkan entry point and no driver function at all - the only thing it calls outside
/// this tree is miniaudio - which is the one fact that makes the audio area's checks headless. On any error
/// everything created so far is retired: the block freed through miniaudio's own free, the record freed and
/// the slot's record field cleared BEFORE the record is freed, the rule every close in this tree states, and
/// *outHandle is left naming nothing live. XUD_INVALID_ARG on a NULL out-handle, table or description, or a
/// description this rung's own check refuses; XUD_UNSUPPORTED on a buffer this rung's decoder cannot read as
/// a container or on a decoded stream past this rung's ceiling; XUD_OUT_OF_MEMORY when the record or the block
/// cannot be spent; XUD_INTERNAL_ERROR on the handle layer's own slot bound.
XudResult xud_audio_open(XudHandle* outHandle, XudHandleTable* table, const XudAudioData* data);

/// Driver: one stream's record freed and its slot released. The handle is validated against the AUDIOSTREAM
/// kind first, so a stale handle, a handle of another kind and a fabricated one refuse without freeing anything;
/// the slot's record field is cleared BEFORE the record is freed, so a close run twice frees once and the table's
/// own teardown cannot find a record it would have to free twice. The block goes before the record because the
/// record names the block, and the block goes through miniaudio's own free for the reason the record's type note
/// gives. XUD_INVALID_ARG on a NULL table or on a handle that does not name a live AUDIOSTREAM slot of this table.
XudResult xud_audio_close(XudHandleTable* table, XudHandle handle);

/// Teardown: every AUDIOSTREAM record of this table freed, above the table's own close and below it, for the
/// reason every other close_all in this tree states - the table's close frees the array, not what the array points
/// at, and a record left behind in a slot a caller released is a leak that close cannot see. The walk is bounded by
/// the array's own high mark rather than by its capacity, so the rows past the last slot ever handed out are not read.
/// XUD_INVALID_ARG on a NULL table or out-closed; XUD_OK with a count of zero on a table whose AUDIOSTREAM array was
/// never allocated, the same answer xud_handle_table_live_count() gives for the same reason.
XudResult xud_audio_close_all(XudHandleTable* table, uint32_t* outClosed);

#endif /* XUD_XUD_AUDIO_H_ */
