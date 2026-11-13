/***********************************************************************************************
 *
 *   xud_test_audio.c - the audio rung and the public audio API above it
 *
 *   Owns : the description's argument check and its two bounds, miniaudio's codes turned into
 *          this library's, the frame-to-bytes stride arithmetic and its ceiling, the two conver
 *          sions between frames and seconds, the decode run over seven real audio files of four
 *          containers and the two refusals, through a bare handle table, the frame count and the
 *          count read back out of the record beside the ones the container's own header declares
 *          them, the public load/close pair and the two queries against a shell context and ag
 *          ainst a real one, the play/stop pair's validation against a shell and its three states
 *          against a real context, the natural end of a stream on the audio thread and the close it
 *          leaves behind, the positional pair's rolloff and pan as pure arithmetic and its place and
 *          base against a shell, the listener and what a placed row hears against a real context, and
 *          the teardown rung above the table's own close - and nothing
 *          else: the slot arrays and the generation bumps are the handle area's, the context's own
 *          bring-up and teardown are the core area's, and the vendor area's miniaudio check is o
 *          nly that the library links, which is not that this rung reads a container.
 *
 *   Layer: tests, above the handle area and below the core area. The whole area but the last three
 *          halves runs with no driver present at all, and that is the point of it rather than a conven
 *          ience: the audio rung's driver half calls no Vulkan entry point and no driver function,
 *          so a decode is miniaudio reading a caller's bytes into a heap block, and a bare handle t
 *          able is one host allocation - which makes the decode, the record, the refusals and the te
 *          ardown rung all checkable headless where the model's and the font areas needed a device to
 *          see anything at all. The GPU half runs the same public pair against a real context, which i
 *          s where the numbers the pure half read out of the record are seen again through the public
 *          queries, one decode rather than two hopes. The play/stop pair is the reason a third half
 *          landed: its validation is answerable against a shell because the pair validates the handle
 *          BEFORE the sound rung is spent, and its three states are not answerable at all without an
 *          audio backend, so the states are gated beside the GPU half rather than counted as a hope -
 *          and the gated half accepts the backend's absence as an answer, which is the one place in th
 *          is area where an environment question rather than a bug can change a code.
 *
 *   What the checks are stated against: the container's own header rather than a table of decode res
 *          ults someone copied - four of the six files are canonical RIFF PCM, so their fmt chunk decla
 *          res the channel count and the rate and their data chunk declares the byte count, and the frame
 *          count of a PCM wav is that byte count divided by the stride those two declare, which is a fact
 *          about the FILE that the decoder has to agree with rather than a fact about the decoder the file
 *          has to agree with. The two compressed files are stated weaker for the reason their container d
 *          oes not declare a PCM size at all: a rate above zero, a channel count inside this rung's bound
 *          and a frame count above zero. The refusals are stated against the split between a descriptio
 *          n's bound and a decoder's refusal, which is the one thing this rung adds on top of miniaudio: a
 *          twelve-byte fragment of a real wav passes this rung's check and then refuses at the decoder, and
 *          a claimed length past the container bound refuses at the check without a byte being read. The
 *          pure half's own numbers are arithmetic on this rung's bounds rather than the files' answers, so
 *          the pure half runs with no file present at all.
 *
 *   Deterministic: nine fixed files, one fixed fragment length, one fixed claimed length, no randomness,
 *   and the two containers this vendored miniaudio still cannot read are asked about rather than skipped,
 *   so a decoder that gained qoa support would fail this area rather than quietly pass it. The one wait
 *   the device half runs is a poll with a fixed tick and a fixed ceiling, which waits for the state
 *   change it checks rather than assuming a wall time the audio thread is not bound to.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#define _POSIX_C_SOURCE 200112L /* Required for: nanosleep under -std=c11, where time.h alone does not declare the POSIX names */

#include <stdio.h> /* Required for: fopen, fread, fclose, ftell, fseek and printf, the six audio files both halves read */
#include <stdlib.h> /* Required for: malloc and free, the file buffers and the bare table */
#include <string.h> /* Required for: memset, memcpy, the shells and the handles before they are filled */
#include <time.h> /* Required for: nanosleep, the tick the natural-end poll sleeps between its questions */

#include "xud.h" /* Required for: XudResult, XudHandle, XudContext, XudAudioData, XudAudioInfo, XUD_HANDLE_KIND_AUDIOSTREAM - the public surface above */
#include "xud_audio.h" /* Required for: XudAudioStreamRecord and the rung's own check/codes/stride/conversions/access/open/close/close_all halves */
#include "xud_core.h" /* Required for: struct XudContext, whose handleTable the public calls reach */
#include "xud_handle.h" /* Required for: XudHandleTable, xud_handle_table_open/close, xud_handle_table_live_count - the slot this record lives in */
#include "xud_sound.h" /* Required for: XudSound and xud_sound_close - the sound rung the public play/stop pair spends, and the one rung this area closes by hand beside the context's own teardown */
#include "xud_test_harness.h" /* Required for: xud_check, xud_gpu_tests_enabled, the area entry points */

/*-----------------------------------------------------------------------------------------------
 * The six audio files, of four containers, and what each one is the check below stated agains
 *
 *   Four are canonical RIFF PCM, which is what makes their header a declaration this rung's dec
 *   oder can be checked AGAIN rather than a result someone copied out of a decoder: the fmt chunk
 *   states the channel count and the rate and the data chunk states the byte count, so the frame
 *   count of one of these is arithmetic on the file's own numbers. One is flac, one is mp3 and one
 *   is ogg, and none of the three declares a PCM size, so all three are stated weaker for that
 *   reason rather than for convenience. The two that are asked about and refused - qoa and xm -
 *   are refused because the vendored miniaudio inlines dr_wav, dr_flac, dr_mp3 and stb_vorbis and
 *   nothing else, which is why this library has no audio-format token: the containers it cannot
 *   read are a property of the vendored copy rather than a choice this rung made.
 *-----------------------------------------------------------------------------------------------*/

#define XUD_TEST_AUDIO_WAV_A "resources/audio/coin.wav" /* canonical RIFF PCM, 8-bit mono 22050 Hz */
#define XUD_TEST_AUDIO_WAV_B "resources/audio/sound.wav" /* canonical RIFF PCM, 32-bit float mono 44100 Hz */
#define XUD_TEST_AUDIO_WAV_C "resources/audio/spring.wav" /* canonical RIFF PCM, 16-bit mono 22050 Hz */
#define XUD_TEST_AUDIO_WAV_D "resources/audio/weird.wav" /* canonical RIFF PCM, 8-bit mono 22050 Hz, a different body */
#define XUD_TEST_AUDIO_FLAC "resources/audio/target.flac" /* flac, two channels, one take of the target track */
#define XUD_TEST_AUDIO_MP3 "resources/audio/country.mp3" /* mp3, two channels, the largest of the nine */
#define XUD_TEST_AUDIO_OGG "resources/audio/target.ogg" /* the same take behind the ogg wrapper, through stb_vorbis */
#define XUD_TEST_AUDIO_QOA "resources/audio/target.qoa" /* a container the vendored miniaudio has no decoder for */
#define XUD_TEST_AUDIO_XM "resources/audio/mini1111.xm" /* ditto */

#define XUD_TEST_AUDIO_COUNT 9u /* the seven that decode and the two that refuse */
#define XUD_TEST_AUDIO_WAV_COUNT 4u /* the ones whose header declares a PCM size, and so the ones with a strong check */
#define XUD_TEST_AUDIO_DECODED_COUNT 7u /* the four wavs, the flac, the mp3 and the ogg, and so the loop bound below */
#define XUD_TEST_AUDIO_FLAC_INDEX 4u /* the flac's slot, and the take the ogg's numbers are compared against below */
#define XUD_TEST_AUDIO_OGG_INDEX 6u /* the same take behind the other wrapper */
#define XUD_TEST_AUDIO_FRAGMENT                                                                                                            \
    12u /* a length that passes this rung's check and then refuses at the decoder - the split this rung adds on top of miniaudio */

/* The pure half's own numbers, and why they are arithmetic rather than the files' answers: the
   pure half runs with no file present at all, so its bounds have to be stated against this rung's
   own limits rather than against a container that may not be on this machine.
*/
#define XUD_TEST_AUDIO_RATE 44100u /* one rate inside this rung's bound, and the one every conversion below is stated at */
#define XUD_TEST_AUDIO_ONE_SECOND 44100ull /* one second of that rate, which is the frame count the divide below has to answer */

static uint8_t* xudTestAudioFile[XUD_TEST_AUDIO_COUNT] = { NULL };
static uint32_t xudTestAudioBytes[XUD_TEST_AUDIO_COUNT] = { 0u };
static uint32_t xudTestAudioWavChannels[XUD_TEST_AUDIO_WAV_COUNT] = { 0u };
static uint32_t xudTestAudioWavRate[XUD_TEST_AUDIO_WAV_COUNT] = { 0u };
static uint64_t xudTestAudioWavFrames[XUD_TEST_AUDIO_WAV_COUNT] = { 0u };
static uint32_t xudTestAudioWavDataBytes[XUD_TEST_AUDIO_WAV_COUNT] = { 0u };
static uint32_t xudTestAudioWavBits[XUD_TEST_AUDIO_WAV_COUNT] = { 0u };

static const char* const xudTestAudioPath[XUD_TEST_AUDIO_COUNT] = { XUD_TEST_AUDIO_WAV_A, XUD_TEST_AUDIO_WAV_B, XUD_TEST_AUDIO_WAV_C,
    XUD_TEST_AUDIO_WAV_D, XUD_TEST_AUDIO_FLAC, XUD_TEST_AUDIO_MP3, XUD_TEST_AUDIO_OGG, XUD_TEST_AUDIO_QOA, XUD_TEST_AUDIO_XM };

static int xud_test_audio_file_read(void) {
    uint32_t i;
    uint32_t read = 0u;

    for (i = 0u; i < XUD_TEST_AUDIO_COUNT; i++) {
        FILE* file = NULL;
        long size = 0L;
        uint8_t* buffer = NULL;

        file = fopen(xudTestAudioPath[i], "rb");
        if (file == NULL) {
            continue;
        }
        if (fseek(file, 0L, SEEK_END) != 0) {
            (void)fclose(file);
            continue;
        }
        size = ftell(file);
        if (size <= 0L) {
            (void)fclose(file);
            continue;
        }
        rewind(file);
        buffer = (uint8_t*)malloc((size_t)size);
        if (buffer == NULL) {
            (void)fclose(file);
            continue;
        }
        if (fread(buffer, 1u, (size_t)size, file) != (size_t)size) {
            free(buffer);
            (void)fclose(file);
            continue;
        }
        (void)fclose(file);
        xudTestAudioFile[i] = buffer;
        xudTestAudioBytes[i] = (uint32_t)size;
        read++;
    }
    return (read == XUD_TEST_AUDIO_COUNT) ? 1 : 0;
}

static void xud_test_audio_file_drop(void) {
    uint32_t i;
    for (i = 0u; i < XUD_TEST_AUDIO_COUNT; i++) {
        free(xudTestAudioFile[i]);
        xudTestAudioFile[i] = NULL;
        xudTestAudioBytes[i] = 0u;
    }
}

/*-----------------------------------------------------------------------------------------------
 * The header reader, and why it is written here rather than taken from the decoder
 *
 *   Because the point of the check below is that the decoder agrees with the CONTAINER rather
 *   than that the container agrees with the decoder, the header has to be read by something in
 *   this file. A canonical RIFF wav is four bytes of 'RIFF', a size, four of 'WAVE', then chunk
 *   s: a 'fmt ' chunk of sixteen bytes whose third field is the channel count, whose fourth is
 *   the rate and whose last is the bits per sample, and a 'data' chunk whose size is the byte
 *   count of the body. Everything little-endian and unaligned, which is why the reads are byte
 *   by byte rather than a cast.
 *-----------------------------------------------------------------------------------------------*/

static uint32_t xud_test_audio_le32(const uint8_t* at) {
    return ((uint32_t)at[0]) | ((uint32_t)at[1] << 8u) | ((uint32_t)at[2] << 16u) | ((uint32_t)at[3] << 24u);
}

static uint32_t xud_test_audio_le16(const uint8_t* at) { return ((uint32_t)at[0]) | ((uint32_t)at[1] << 8u); }

static int xud_test_audio_wav_expect(
    uint32_t index, uint32_t* outChannels, uint32_t* outRate, uint32_t* outBits, uint32_t* outDataBytes, uint64_t* outFrames) {
    const uint8_t* bytes = NULL;
    uint32_t size = 0u;
    uint32_t at = 0u;
    uint32_t channels = 0u;
    uint32_t rate = 0u;
    uint32_t bits = 0u;
    uint32_t dataBytes = 0u;

    *outChannels = 0u;
    *outRate = 0u;
    *outBits = 0u;
    *outDataBytes = 0u;
    *outFrames = 0u;

    bytes = xudTestAudioFile[index];
    size = xudTestAudioBytes[index];
    if ((size < 12u) || (memcmp(bytes, "RIFF", 4u) != 0) || (memcmp(bytes + 8u, "WAVE", 4u) != 0)) {
        return 0;
    }

    at = 12u;
    while ((at + 8u) <= size) {
        const uint32_t chunkSize = xud_test_audio_le32(bytes + at + 4u);
        if (memcmp(bytes + at, "fmt ", 4u) == 0) {
            if ((chunkSize < 16u) || ((at + 8u + chunkSize) > size)) {
                return 0;
            }
            if (xud_test_audio_le16(bytes + at + 8u) != 1u) {
                return 0; /* not uncompressed PCM, so the frame arithmetic below has no meaning for it */
            }
            channels = xud_test_audio_le16(bytes + at + 10u);
            rate = xud_test_audio_le32(bytes + at + 12u);
            bits = xud_test_audio_le16(bytes + at + 22u);
            if ((channels == 0u) || (rate == 0u) || (bits != 8u && bits != 16u && bits != 32u)) {
                return 0; /* only the three widths this decoder can widen to this rung's are stated below */
            }
        } else if (memcmp(bytes + at, "data", 4u) == 0) {
            dataBytes = chunkSize;
            if ((at + 8u + chunkSize) > size) {
                return 0;
            }
        }
        if (chunkSize == 0u) {
            return 0;
        }
        at += 8u + chunkSize + (chunkSize & 1u); /* chunks are word-padded, so an odd size skips its pad byte */
    }

    if ((channels == 0u) || (rate == 0u) || (bits != 8u && bits != 16u && bits != 32u) || (dataBytes == 0u)) {
        return 0;
    }

    *outChannels = channels;
    *outRate = rate;
    *outBits = bits;
    *outDataBytes = dataBytes;
    *outFrames = (uint64_t)dataBytes / (((uint64_t)channels * (uint64_t)bits) / 8u);
    return 1;
}

static void xud_test_audio_expect(void) {
    uint32_t i;
    uint32_t strong = 0u;

    for (i = 0u; i < XUD_TEST_AUDIO_WAV_COUNT; i++) {
        if (xud_test_audio_wav_expect(i, &xudTestAudioWavChannels[i], &xudTestAudioWavRate[i], &xudTestAudioWavBits[i],
                &xudTestAudioWavDataBytes[i], &xudTestAudioWavFrames[i])
            != 0) {
            strong++;
        }
    }
    xud_check(strong == XUD_TEST_AUDIO_WAV_COUNT, "expect-all-canonical",
        "all four wavs are canonical RIFF uncompressed PCM, which is what makes the strong checks below meaningful rather than vacuous");
}

/*-----------------------------------------------------------------------------------------------
 * The description's check, and the two bounds it is stated against
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_audio_check(void) {
    XudAudioData empty;
    XudAudioData claimed;
    XudAudioData good;
    static const uint8_t synthetic[16] = { 'R', 'I', 'F', 'F', 4u, 0u, 0u, 0u, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' };

    memset(&empty, 0, sizeof(empty));
    memset(&claimed, 0, sizeof(claimed));
    memset(&good, 0, sizeof(good));

    xud_check(xud_audio_check(NULL) == XUD_INVALID_ARG, "check-null", "a description with nothing in it is refused");

    xud_check(xud_audio_check(&empty) == XUD_INVALID_ARG, "check-no-bytes",
        "and so is one that names no bytes, which is the same answer as a NULL fileData and is stated as one rather than two");
    empty.fileData = synthetic;
    xud_check(xud_audio_check(&empty) == XUD_INVALID_ARG, "check-bytes-unclaimed",
        "and a buffer named without its length is a description this rung cannot lay out either");
    empty.fileBytes = XUD_TEST_AUDIO_FRAGMENT - 1u;
    xud_check(xud_audio_check(&empty) == XUD_INVALID_ARG, "check-fragment",
        "bytes shorter than the shortest header this decoder sniffs are refused as the description's fault rather than as the decoder's");
    empty.fileBytes = XUD_TEST_AUDIO_FRAGMENT;
    xud_check(xud_audio_check(&empty) == XUD_OK, "check-fragment-bound",
        "one byte more is inside this rung's bound and is refused further down instead, which is where the split between a bound and a "
        "refusal is");

    claimed.fileData = synthetic;
    claimed.fileBytes = XUD_AUDIO_MAX_FILE_BYTES + 1u;
    xud_check(xud_audio_check(&claimed) == XUD_INVALID_ARG, "check-past-bound",
        "a claimed length past the container bound is refused here rather than at the decoder, which is the only place this rung can bound "
        "what it will spend");
    claimed.fileBytes = XUD_AUDIO_MAX_FILE_BYTES;
    xud_check(xud_audio_check(&claimed) == XUD_OK, "check-bound-exact",
        "and the bound is inclusive, so the refusal is a bound rather than a range that stops one short of its own limit");

    good.fileData = synthetic;
    good.fileBytes = (uint32_t)sizeof(synthetic);
    xud_check(xud_audio_check(&good) == XUD_OK, "check-good",
        "a description of a real length and a real buffer is one this rung can lay out, and the check says nothing about its content");
}

/*-----------------------------------------------------------------------------------------------
 * miniaudio's answer as this library's code
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_audio_codes(void) {
    xud_check(xud_audio_result_from_ma(MA_SUCCESS) == XUD_OK, "code-ok", "the decoder's success is this library's success");
    xud_check(xud_audio_result_from_ma(MA_OUT_OF_MEMORY) == XUD_OUT_OF_MEMORY, "code-oom",
        "and its out-of-memory is this library's, because its allocator is libc's rather than a driver's");
    xud_check(xud_audio_result_from_ma(MA_TOO_BIG) == XUD_UNSUPPORTED, "code-too-big",
        "miniaudio's own size bound is a refusal of a well-formed description rather than a bad argument, the same code a set that fits no "
        "atlas gets");
    xud_check(xud_audio_result_from_ma(MA_INVALID_FILE) == XUD_UNSUPPORTED, "code-invalid-file",
        "a buffer that is not a container this decoder reads");
    xud_check(xud_audio_result_from_ma(MA_INVALID_DATA) == XUD_UNSUPPORTED, "code-invalid-data",
        "and one that is a container but cannot be walked is the SAME answer, which is the reason the font rung gives for the same pair");
    xud_check(xud_audio_result_from_ma(MA_CRC_MISMATCH) == XUD_UNSUPPORTED, "code-crc",
        "and so for a container whose own checksum disagrees with its body");
    xud_check(xud_audio_result_from_ma(MA_FORMAT_NOT_SUPPORTED) == XUD_UNSUPPORTED, "code-format",
        "and so for a container this vendored copy has no decoder for at all");
    xud_check(xud_audio_result_from_ma(MA_NO_BACKEND) == XUD_UNSUPPORTED, "code-no-backend", "and so for a decoder that was not built in");
    xud_check(xud_audio_result_from_ma(MA_BACKEND_NOT_ENABLED) == XUD_UNSUPPORTED, "code-backend-off",
        "and so for one that was built in but not enabled, which is the same shape as the one above and is not a second code");
    xud_check(xud_audio_result_from_ma(MA_NO_DEVICE) == XUD_UNSUPPORTED, "code-no-device",
        "and so for a device that is not there, which is the one code the decoder half below will never answer");
    xud_check(xud_audio_result_from_ma(MA_INVALID_OPERATION) == XUD_UNSUPPORTED, "code-invalid-operation",
        "and so for a decoder asked to do something it is not in a state to do");
    xud_check(xud_audio_result_from_ma(MA_OUT_OF_RANGE) == XUD_UNSUPPORTED, "code-out-of-range",
        "and so for a value outside the decoder's own range");
    xud_check(xud_audio_result_from_ma(MA_NOT_IMPLEMENTED) == XUD_UNSUPPORTED, "code-not-implemented",
        "and so for a decoder that answers not-implemented rather than an answer");
    xud_check(xud_audio_result_from_ma(MA_ERROR) == XUD_UNSUPPORTED, "code-generic",
        "and so for the decoder's generic error, which is the loosest of the refusals and therefore the last of them rather than a code of "
        "its own");
    xud_check(xud_audio_result_from_ma(MA_INVALID_ARGS) == XUD_INTERNAL_ERROR, "code-invalid-args",
        "a decoder-level bad argument is NOT the caller's: this rung builds the config it hands the decoder itself, so a decoder calling "
        "that bad is a rung that built the wrong config");
    xud_check(xud_audio_result_from_ma((ma_result)-12345) == XUD_INTERNAL_ERROR, "code-unmapped",
        "and an unmapped shape refuses rather than passing, which is what makes the default this library's own code rather than the "
        "decoder's");
}

/*-----------------------------------------------------------------------------------------------
 * The stride arithmetic, stated against this rung's own bounds
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_audio_stride(void) {
    size_t bytes = 0u;

    xud_check(
        xud_audio_bytes_of_frames(NULL, 1u, 1u) == XUD_INVALID_ARG, "stride-null-out", "a size with nowhere to be written is refused");

    xud_check(
        xud_audio_bytes_of_frames(&bytes, 0u, 1u) == XUD_OK, "stride-empty-ok", "a stream of no frames is a size this rung will hold");
    xud_check(bytes == 0u, "stride-empty",
        "and answers no bytes rather than an absent block, which is what makes an empty stream an empty stream rather than a missing one");

    xud_check(xud_audio_bytes_of_frames(&bytes, 1u, 0u) == XUD_UNSUPPORTED, "stride-no-channels",
        "a channel count of zero is a stream with no samples per frame, which is a stream this rung cannot own rather than an empty one");
    xud_check(xud_audio_bytes_of_frames(&bytes, 1u, XUD_AUDIO_MAX_CHANNELS) == XUD_OK, "stride-channels-exact",
        "the largest channel count this rung will hold is admitted, so the bound is a bound rather than a range that stops short");
    xud_check(xud_audio_bytes_of_frames(&bytes, 1u, XUD_AUDIO_MAX_CHANNELS + 1u) == XUD_UNSUPPORTED, "stride-past-channels",
        "and one past it is refused, which is a stream with no mixer path here");

    xud_check(xud_audio_bytes_of_frames(&bytes, XUD_TEST_AUDIO_ONE_SECOND, 2u) == XUD_OK, "stride-one-second-ok",
        "one second of stereo at the test rate is a size this rung will hold");
    xud_check(bytes == (size_t)(XUD_TEST_AUDIO_ONE_SECOND * 2ull * (uint64_t)XUD_AUDIO_BYTES_PER_SAMPLE), "stride-one-second-value",
        "and answers the product the record's own two numbers declare, which is the stride every walk below is bounded by");

    xud_check(xud_audio_bytes_of_frames(&bytes, 0xFFFFFFFFFFFFFFFFull, 1u) == XUD_UNSUPPORTED, "stride-huge",
        "the widest frame count is refused rather than overflowing, which is why the bound is stated as a division rather than as a "
        "multiply");
    xud_check(xud_audio_bytes_of_frames(&bytes, XUD_AUDIO_MAX_FRAMES, 1u) == XUD_OK, "stride-ceiling-exact",
        "a one-channel stream at the frame ceiling is inside the PCM bound, which is the loosest of the pair");
    xud_check(xud_audio_bytes_of_frames(&bytes, XUD_AUDIO_MAX_FRAMES + 1ull, 1u) == XUD_UNSUPPORTED, "stride-past-ceiling",
        "and one frame past it is refused, so the ceiling is a bound rather than a range that stops short");
    xud_check(xud_audio_bytes_of_frames(&bytes, XUD_AUDIO_MAX_FRAMES, 2u) == XUD_UNSUPPORTED, "stride-two-channel-ceiling",
        "the same frame count at two channels is refused, which is the reason the ceiling is stated in bytes rather than in frames");
}

/*-----------------------------------------------------------------------------------------------
 * The two conversions between frames and seconds, and the pair they are of each other
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_audio_time(void) {
    double seconds = 0.0;
    uint64_t frames = 0u;

    xud_check(xud_audio_seconds_from_frames(NULL, 1u, 1u) == XUD_INVALID_ARG, "seconds-null-out",
        "a length with nowhere to be written is refused");
    xud_check(xud_audio_seconds_from_frames(&seconds, 1u, 0u) == XUD_UNSUPPORTED, "seconds-no-rate",
        "a record that never had a rate is not a record this rung can divide against, the same answer the clock rung gives for a frequency "
        "of zero");

    xud_check(xud_audio_seconds_from_frames(&seconds, XUD_TEST_AUDIO_ONE_SECOND, XUD_TEST_AUDIO_RATE) == XUD_OK, "seconds-one-ok",
        "one second of the test rate answers");
    xud_check(seconds == 1.0, "seconds-one-value",
        "and answers exactly one, which is the divide rather than a length stored somewhere that could disagree with the count");

    xud_check(xud_audio_seconds_from_frames(&seconds, 0u, XUD_TEST_AUDIO_RATE) == XUD_OK, "seconds-empty-ok",
        "no frames is a length this rung answers");
    xud_check(seconds == 0.0, "seconds-empty", "and answers no length rather than an absent one");

    xud_check(xud_audio_frames_from_seconds(NULL, 1.0, 1u) == XUD_INVALID_ARG, "frames-null-out",
        "a frame count with nowhere to be written is refused");
    xud_check(xud_audio_frames_from_seconds(&frames, 1.0, 0u) == XUD_UNSUPPORTED, "frames-no-rate",
        "and so for a rate to divide by that does not exist");
    xud_check(xud_audio_frames_from_seconds(&frames, -1.0, XUD_TEST_AUDIO_RATE) == XUD_UNSUPPORTED, "frames-negative",
        "a negative span is refused rather than wrapped, which is a sound placed at the end of every stream rather than a sound that never "
        "started");

    xud_check(xud_audio_frames_from_seconds(&frames, 1.0, XUD_TEST_AUDIO_RATE) == XUD_OK, "frames-one-ok",
        "one second of the test rate answers a frame count");
    xud_check(frames == XUD_TEST_AUDIO_ONE_SECOND, "frames-one-value",
        "and answers the count the divide above was stated against, which is what makes the two one pair rather than two divisions");

    xud_check(xud_audio_frames_from_seconds(&frames, 0.5, XUD_TEST_AUDIO_RATE) == XUD_OK, "frames-half-ok",
        "half a second of the test rate answers a frame count");
    xud_check(frames == (uint64_t)(0.5 * (double)XUD_TEST_AUDIO_RATE), "frames-half-value",
        "and truncates toward zero rather than away from it, which is the direction the divide above truncates in as well");

    xud_check(xud_audio_frames_from_seconds(&frames, 0.0, XUD_TEST_AUDIO_RATE) == XUD_OK, "frames-zero-ok",
        "no span is a count this rung answers");
    xud_check(frames == 0u, "frames-zero", "and answers no frames");

    xud_check(xud_audio_frames_from_seconds(&frames, 1.0e12, XUD_TEST_AUDIO_RATE) == XUD_UNSUPPORTED, "frames-past-ceiling",
        "a span this rung's own frame ceiling cannot name is refused, and is compared in the double rather than after a cast, which is "
        "what makes it a refusal rather than undefined behaviour");

    xud_check(xud_audio_frames_from_seconds(&frames, 0.5, XUD_TEST_AUDIO_RATE) == XUD_OK, "round-trip-ok", "half a second answers a count");
    xud_check(xud_audio_seconds_from_frames(&seconds, frames, XUD_TEST_AUDIO_RATE) == XUD_OK, "round-trip-inverse",
        "and the inverse of that count answers a length");
    xud_check(seconds <= 0.5, "round-trip-under",
        "and answers no more than the span it came from, which is the one fact that makes the two conversions one pair rather than two "
        "divisions");
}

/*-----------------------------------------------------------------------------------------------
 * The decode, against a bare handle table
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_audio_decode(void) {
    XudHandleTable* table = NULL;
    XudHandle handle = { 0u, 0u, 0u };
    XudAudioData audio;
    const XudAudioStreamRecord* record = NULL;
    size_t stride = 0u;
    uint32_t closed = 0u;
    uint32_t live = 0u;
    uint32_t i;
    uint32_t flacChannels = 0u;
    uint32_t flacRate = 0u;
    uint64_t flacFrames = 0u;
    uint32_t oggChannels = 0u;
    uint32_t oggRate = 0u;
    uint64_t oggFrames = 0u;

    memset(&handle, 0, sizeof(handle));
    memset(&audio, 0, sizeof(audio));

    xud_check(xud_handle_table_open(&table) == XUD_OK, "decode/table",
        "a bare handle table is one host allocation and calls no driver function, which is what makes a decode checkable with no device in "
        "sight");
    if (table == NULL) {
        return;
    }

    for (i = 0u; i < XUD_TEST_AUDIO_WAV_COUNT; i++) {
        XudResult result = XUD_INTERNAL_ERROR;

        if (xudTestAudioWavFrames[i] == 0u) {
            continue; /* not a canonical container, so there is nothing strong to state about it */
        }

        audio.fileData = xudTestAudioFile[i];
        audio.fileBytes = xudTestAudioBytes[i];
        result = xud_audio_open(&handle, table, &audio);
        xud_check(result == XUD_OK, "decode-wav-ok",
            "a real canonical RIFF PCM file decodes through a bare table, with no device and no loader in sight");
        if (result != XUD_OK) {
            continue;
        }
        xud_check(handle.kind == XUD_HANDLE_KIND_AUDIOSTREAM, "decode-wav-kind",
            "and the handle it hands out is tagged AUDIOSTREAM, which is what makes a font handle refused by this close");
        xud_check(xud_audio_get(table, handle, &record) == XUD_OK, "decode-wav-get",
            "and the record behind the slot is readable, which is what the two public queries above reach through");
        if (record == NULL) {
            continue;
        }

        /* The three numbers are the file's own header's answers rather than the decoder's, which
           is the direction the check is stated in: a decoder that got the channel count wrong or
           the rate wrong fails here rather than agreeing with itself.
        */
        xud_check(record->channels == xudTestAudioWavChannels[i], "decode-wav-channels",
            "the channel count is the one the file's own fmt chunk declares");
        xud_check(record->sampleRate == xudTestAudioWavRate[i], "decode-wav-rate", "and so is the rate");
        xud_check(record->frameCount == xudTestAudioWavFrames[i], "decode-wav-frames",
            "and the frame count is that byte count divided by the stride those two declare, which is arithmetic on the file rather than a "
            "copied result");
        xud_check(record->frames != NULL, "decode-wav-block",
            "and the block itself exists, which is the one thing the three numbers above do not prove");

        xud_check(xud_audio_bytes_of_frames(&stride, record->frameCount, record->channels) == XUD_OK, "decode-wav-stride",
            "the stride of what the decoder answered is a block this rung will hold");
        xud_check((uint64_t)stride == record->frameCount * (uint64_t)record->channels * (uint64_t)XUD_AUDIO_BYTES_PER_SAMPLE,
            "decode-wav-stride-value",
            "and answers the product the record's own two numbers declare, which is the stride every walk below is bounded by");
        xud_check((uint64_t)stride
                == (uint64_t)xudTestAudioWavDataBytes[i] * ((uint64_t)XUD_AUDIO_BYTES_PER_SAMPLE * 8u / (uint64_t)xudTestAudioWavBits[i]),
            "decode-wav-stride-widened",
            "and answers the container's own data byte count widened by the ratio of this rung's sample width to the container's own bit "
            "depth, which is the one tie between this rung's arithmetic and the container's - and the reason an 8-bit wav spends four "
            "bytes a sample here rather than one");

        xud_check(xud_audio_close(table, handle) == XUD_OK, "decode-wav-close", "the stream closes and its block and record go back");
        memset(&handle, 0, sizeof(handle));
        record = NULL;
    }

    for (i = XUD_TEST_AUDIO_WAV_COUNT; i < XUD_TEST_AUDIO_DECODED_COUNT; i++) {
        XudResult result = XUD_INTERNAL_ERROR;

        audio.fileData = xudTestAudioFile[i];
        audio.fileBytes = xudTestAudioBytes[i];
        result = xud_audio_open(&handle, table, &audio);
        xud_check(result == XUD_OK, "decode-compressed-ok",
            "a real flac, a real mp3 and a real ogg decode through the same path as the wavs, which is the reason there is no per-format "
            "loader name here");
        if (result != XUD_OK) {
            continue;
        }
        if (xud_audio_get(table, handle, &record) == XUD_OK) {
            xud_check(record->sampleRate > 0u, "decode-compressed-rate",
                "the rate is answered rather than left at the sentinel, which is what makes the divide above a refusal rather than a "
                "division by zero");
            xud_check(record->channels >= 1u && record->channels <= XUD_AUDIO_MAX_CHANNELS, "decode-compressed-channels",
                "and the channel count is inside this rung's bound, which is the bound the stride above refused past");
            xud_check(record->frameCount > 0u, "decode-compressed-frames",
                "and the frame count is answered rather than left at zero, which is the one shape that would make a stream of no length "
                "look like a stream that never decoded");
            xud_check(record->frames != NULL, "decode-compressed-block",
                "and the block exists, which is the pair of the frame count rather than a fact about its size");
            if (i == XUD_TEST_AUDIO_FLAC_INDEX) {
                flacChannels = record->channels;
                flacRate = record->sampleRate;
                flacFrames = record->frameCount;
            }
            if (i == XUD_TEST_AUDIO_OGG_INDEX) {
                oggChannels = record->channels;
                oggRate = record->sampleRate;
                oggFrames = record->frameCount;
            }
        }
        xud_check(xud_audio_close(table, handle) == XUD_OK, "decode-compressed-close",
            "and it closes, so the block a compressed file spent is not left behind in a slot");
        memset(&handle, 0, sizeof(handle));
        record = NULL;
    }

    /* The same take behind two wrappers, and the reason the two answers have to agree: target.flac
       and target.ogg are one recording carried by two containers, walked here by dr_flac and by
       stb_vorbis, so the three numbers are a fact about the recording rather than a fact about
       whichever decoder walked it - the one cross-decoder statement this area can make. */
    xud_check((flacChannels == oggChannels) && (flacRate == oggRate) && (flacFrames == oggFrames), "decode-two-wrappers-agree",
        "and the flac and the ogg of one take answer the same channel count, rate and frame count, which is the one fact stated across two "
        "decoders here");

    for (i = XUD_TEST_AUDIO_DECODED_COUNT; i < XUD_TEST_AUDIO_COUNT; i++) {
        audio.fileData = xudTestAudioFile[i];
        audio.fileBytes = xudTestAudioBytes[i];
        xud_check(xud_audio_open(&handle, table, &audio) == XUD_UNSUPPORTED, "decode-unsupported-container",
            "a container this vendored copy has no decoder for refuses rather than handing back a half-decoded stream");
        xud_check(handle.kind == XUD_HANDLE_KIND_NONE, "decode-unsupported-left-handle",
            "and leaves the handle naming nothing, so a refusal spends no slot");
    }

    xud_check(xud_handle_table_live_count(table, XUD_HANDLE_KIND_AUDIOSTREAM, &live) == XUD_OK, "decode-live-query",
        "the AUDIOSTREAM live count answers after every stream above closed");
    xud_check(live == 0u, "decode-live-zero", "and answers zero, so nothing above leaked a slot into the table's own teardown");

    xud_check(xud_audio_close_all(table, &closed) == XUD_OK, "decode-close-all-idempotent",
        "the teardown rung runs with nothing left and is not refused");
    xud_check(closed == 0u, "decode-close-all-count",
        "and it closed nothing, so the rung above the table's close is idempotent rather than a double free waiting for it");

    xud_check(xud_handle_table_close(&table) == XUD_OK, "decode-table-close",
        "and the bare table closes, which frees the arrays but not what they point at - the reason the rung above it exists");
    xud_check(table == NULL, "decode-table-cleared", "and is cleared, so a table torn down twice cannot double-free");
}

/*-----------------------------------------------------------------------------------------------
 * The refusals, and what each one leaves unspent
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_audio_refused(void) {
    XudHandleTable* table = NULL;
    XudHandle handle = { 0u, 0u, 0u };
    XudAudioData audio;
    XudAudioData broken;
    const XudAudioStreamRecord* record = NULL;
    static uint8_t zeros[1024];
    uint8_t fragment[XUD_TEST_AUDIO_FRAGMENT];
    uint32_t closed = 0u;

    memset(&handle, 0, sizeof(handle));
    memset(&audio, 0, sizeof(audio));
    memset(&broken, 0, sizeof(broken));
    memset(zeros, 0, sizeof(zeros));

    xud_check(xud_handle_table_open(&table) == XUD_OK, "refused/table",
        "a second bare table, so the refusals below are counted against an empty one rather than against the decode's leftovers");
    if (table == NULL) {
        return;
    }

    xud_check(xud_audio_open(NULL, table, &audio) == XUD_INVALID_ARG, "refused-null-out", "a handle with nowhere to be written is refused");
    xud_check(
        xud_audio_open(&handle, NULL, &audio) == XUD_INVALID_ARG, "refused-null-table", "a stream with no table to own it is refused");
    xud_check(xud_audio_open(&handle, table, NULL) == XUD_INVALID_ARG, "refused-null-data", "and a stream with no description is refused");
    xud_check(handle.kind == XUD_HANDLE_KIND_NONE, "refused-left-handle",
        "and none of the three spent a slot, which is the rule every create in this tree states");

    audio.fileData = xudTestAudioFile[0];
    audio.fileBytes = 0u;
    xud_check(xud_audio_open(&handle, table, &audio) == XUD_INVALID_ARG, "refused-empty",
        "a description that names no bytes is refused before a slot is spent");
    audio.fileBytes = XUD_TEST_AUDIO_FRAGMENT - 1u;
    xud_check(xud_audio_open(&handle, table, &audio) == XUD_INVALID_ARG, "refused-fragment-short",
        "and bytes shorter than the shortest header this decoder sniffs are refused as the description's fault");

    /* The split this rung adds on top of miniaudio, and the one place it is visible: a twelve-byte
       fragment of a real wav is inside this rung's bound, so the check admits it and spends nothing,
       and the decoder is what refuses it. Two codes, two reasons, and the reason a caller reads them
       differently is the reason they are two rather than one.
    */
    memcpy(fragment, xudTestAudioFile[0], XUD_TEST_AUDIO_FRAGMENT);
    broken.fileData = fragment;
    broken.fileBytes = XUD_TEST_AUDIO_FRAGMENT;
    xud_check(xud_audio_check(&broken) == XUD_OK, "refused-fragment-inside",
        "the fragment is inside this rung's check, which is what makes the refusal below a decoder's rather than a description's");
    xud_check(xud_audio_open(&handle, table, &broken) == XUD_UNSUPPORTED, "refused-fragment-decoder",
        "and the decoder refuses it, so a fragment of a real container is a refusal rather than a half-stream");
    xud_check(handle.kind == XUD_HANDLE_KIND_NONE, "refused-fragment-handle",
        "and the refusal leaves the handle naming nothing, so no slot was spent for a stream that does not exist");

    broken.fileData = zeros;
    broken.fileBytes = (uint32_t)sizeof(zeros);
    xud_check(xud_audio_open(&handle, table, &broken) == XUD_UNSUPPORTED, "refused-zeros",
        "a buffer of nothing at all is refused the same way, which is one answer for a buffer that is not a container and for one it is "
        "but cannot walk");

    broken.fileData = xudTestAudioFile[0];
    broken.fileBytes = XUD_AUDIO_MAX_FILE_BYTES + 1u;
    xud_check(xud_audio_open(&handle, table, &broken) == XUD_INVALID_ARG, "refused-past-bound",
        "a claimed length past the container bound is refused at the check rather than at the decoder, which is the only place this rung "
        "can bound what it will spend");
    xud_check(handle.kind == XUD_HANDLE_KIND_NONE, "refused-past-bound-handle", "and nothing was read or spent for it");

    xud_check(xud_audio_close(NULL, handle) == XUD_INVALID_ARG, "refused-close-null-table", "a close with no table is refused");
    xud_check(xud_audio_close(table, handle) == XUD_INVALID_ARG, "refused-close-nothing",
        "and a close of a handle that never named a slot is refused before anything is freed, which is the same validation the queries "
        "below run");
    xud_check(
        xud_audio_get(table, handle, NULL) == XUD_INVALID_ARG, "refused-get-null-out", "a record with nowhere to be written is refused");
    xud_check(xud_audio_get(table, handle, &record) == XUD_INVALID_ARG, "refused-get-nothing",
        "and so for a handle that names no live slot, which is what makes the two public queries refusals rather than a NULL record a "
        "caller would read");
    xud_check(record == NULL, "refused-get-cleared",
        "and the accessor cleared the out-record rather than leaving it as it was, which is what makes the refusal above readable");

    xud_check(
        xud_audio_close_all(NULL, &closed) == XUD_INVALID_ARG, "refused-close-all-null-table", "a teardown rung with no table is refused");
    xud_check(xud_audio_close_all(table, NULL) == XUD_INVALID_ARG, "refused-close-all-null-count",
        "and so for a count with nowhere to be written, which is the same reason as every other out-parameter in this tree");

    xud_check(xud_handle_table_close(&table) == XUD_OK, "refused-table-close",
        "and the second bare table closes with nothing of this kind ever created in it");
    xud_check(table == NULL, "refused-table-cleared", "and is cleared");
}

/*-----------------------------------------------------------------------------------------------
 * The public surface, against a shell context
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_audio_shell(void) {
    XudContext shell;
    XudContext empty;
    XudHandleTable* table = NULL;
    XudHandle handle = { 0u, 0u, 0u };
    XudHandle stale = { 0u, 0u, 0u };
    XudHandle foreign = { 0u, 0u, 0u };
    XudAudioData audio;
    XudAudioInfo info;
    const XudAudioStreamRecord* record = NULL;
    float seconds = 0.0f;
    uint32_t closed = 0u;
    uint32_t live = 0u;

    memset(&shell, 0, sizeof(shell));
    memset(&empty, 0, sizeof(empty));
    memset(&handle, 0, sizeof(handle));
    memset(&audio, 0, sizeof(audio));
    memset(&info, 0, sizeof(info));

    xud_check(xud_handle_table_open(&table) == XUD_OK, "shell/table",
        "a bare table is the one rung the public audio calls reach, which is the point of the audio rung being driver-free");
    if (table == NULL) {
        return;
    }
    shell.handleTable = table;

    xud_check(xud_load_audio(&handle, &empty, &audio) == XUD_UNSUPPORTED, "shell-no-rungs-below",
        "a context that owns no handle table is refused as UNSUPPORTED rather than INVALID_ARG, the same split every other public pair in "
        "this tree gives");

    audio.fileData = xudTestAudioFile[0];
    audio.fileBytes = xudTestAudioBytes[0];

    xud_check(
        xud_load_audio(NULL, &shell, &audio) == XUD_INVALID_ARG, "shell-load-null-out", "a handle with nowhere to be written is refused");
    xud_check(xud_load_audio(&handle, NULL, &audio) == XUD_INVALID_ARG, "shell-load-null-context",
        "a stream with no context to own it is refused");
    xud_check(
        xud_load_audio(&handle, &shell, NULL) == XUD_INVALID_ARG, "shell-load-null-data", "and a stream with no description is refused");
    xud_check(handle.kind == XUD_HANDLE_KIND_NONE, "shell-load-refusals-spent-nothing", "and none of the three spent a slot");

    xud_check(
        xud_load_audio(&handle, &shell, &audio) == XUD_OK, "shell-load", "the public load decodes a real file through a shell context");
    xud_check(handle.kind == XUD_HANDLE_KIND_AUDIOSTREAM, "shell-load-kind", "and the handle it hands out is tagged AUDIOSTREAM");

    xud_check(xud_get_audio_info(NULL, &shell, handle) == XUD_INVALID_ARG, "shell-info-null-out",
        "a format with nowhere to be written is refused");
    xud_check(xud_get_audio_info(&info, &empty, handle) == XUD_UNSUPPORTED, "shell-info-no-rungs-below",
        "and a context with no table under it is refused as UNSUPPORTED rather than as a bad handle, which is the same split the load "
        "above gave");
    xud_check(xud_get_audio_info(&info, &shell, handle) == XUD_OK, "shell-info", "the public format query answers through the shell");
    xud_check(xud_audio_get(table, handle, &record) == XUD_OK, "shell-record", "and the record behind the slot is readable beside it");
    if (record != NULL) {
        xud_check(info.channels == record->channels, "shell-info-channels",
            "the public format query answers the record's own channel count rather than a second copy of it");
        xud_check(info.sampleRate == record->sampleRate, "shell-info-rate", "and its rate");
        xud_check(info.frameCount == record->frameCount, "shell-info-frames",
            "and its frame count, which is what makes the three one view of one record rather than three hopes");
    }

    xud_check(xud_get_audio_length(NULL, &shell, handle) == XUD_INVALID_ARG, "shell-length-null-out",
        "a length with nowhere to be written is refused");
    xud_check(xud_get_audio_length(&seconds, &shell, handle) == XUD_OK, "shell-length",
        "the public length answers, and it is the pure divide above rather than a length stored at the load");
    xud_check(seconds > 0.0f, "shell-length-positive", "and answers a positive length");
    if (record != NULL) {
        xud_check((double)seconds < 2.0, "shell-length-short",
            "and under two seconds, which is the file's own declared byte count divided by its own declared stride");
        xud_check((double)seconds > ((double)record->frameCount / (double)record->sampleRate) - 0.0001
                && (double)seconds < ((double)record->frameCount / (double)record->sampleRate) + 0.0001,
            "shell-length-matches-record",
            "and answers the record's own count divided by its own rate to within a float's own rounding, which is the divide rather than "
            "a second number that could disagree with the count");
    }

    xud_check(xud_close_audio(&shell, handle) == XUD_OK, "shell-close",
        "the public close releases it, and no wait-idle guard belongs here because no queue was ever asked to read the block being freed");
    xud_check(xud_close_audio(&shell, handle) == XUD_INVALID_ARG, "shell-close-twice",
        "a close run twice is refused by the generation bump rather than double-freeing the block");
    xud_check(xud_audio_close_all(shell.handleTable, &closed) == XUD_OK, "shell-close-all-idempotent",
        "the teardown rung runs again with nothing left and is not refused");
    xud_check(closed == 0u, "shell-close-all-count",
        "and it closed nothing, so the rung above the table's close is idempotent rather than a double free waiting for it");

    stale = handle;
    stale.generation = handle.generation + 1u;
    xud_check(xud_close_audio(&shell, stale) == XUD_INVALID_ARG, "shell-close-stale",
        "a handle kept past the release that freed its slot is refused before anything is freed");
    xud_check(xud_get_audio_info(&info, &shell, stale) == XUD_INVALID_ARG, "shell-info-stale",
        "and so for the format query, which is the same validation the close runs");
    xud_check(xud_get_audio_length(&seconds, &shell, stale) == XUD_INVALID_ARG, "shell-length-stale", "and so for the length query");

    foreign = handle;
    foreign.kind = XUD_HANDLE_KIND_TEXTURE;
    xud_check(xud_close_audio(&shell, foreign) == XUD_INVALID_ARG, "shell-close-foreign-kind",
        "and a handle of another kind is refused rather than freeing a record out of that other kind's array, which is the reason the "
        "AUDIOSTREAM walk below cannot find a thing it should not");

    xud_check(xud_handle_table_live_count(shell.handleTable, XUD_HANDLE_KIND_AUDIOSTREAM, &live) == XUD_OK, "shell-live-query",
        "the AUDIOSTREAM live count answers after the close");
    xud_check(live == 0u, "shell-live-zero", "and answers zero, so the shell left no stream behind");

    xud_check(xud_handle_table_close(&shell.handleTable) == XUD_OK, "shell-table-close", "and the shell's table closes");
    xud_check(shell.handleTable == NULL, "shell-table-cleared",
        "and is cleared through the shell's own copy of the pointer, which is what makes the shell's table a table a second close would "
        "refuse");
}

/*-----------------------------------------------------------------------------------------------
 * The public surface, against a real context
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_audio_gpu(void) {
    XudContext* context = NULL;
    XudHandle handle = { 0u, 0u, 0u };
    XudHandle stale = { 0u, 0u, 0u };
    XudHandle foreign = { 0u, 0u, 0u };
    XudAudioData audio;
    XudAudioInfo info;
    const XudAudioStreamRecord* record = NULL;
    float seconds = 0.0f;
    uint32_t closed = 0u;
    uint32_t live = 0u;

    memset(&handle, 0, sizeof(handle));
    memset(&audio, 0, sizeof(audio));
    memset(&info, 0, sizeof(info));

    xud_check(xud_init_context(&context, 64u, 64u, "xud_test") == XUD_OK, "gpu-context",
        "a context comes up for the audio half, which is the one thing a decode does NOT need and a public handle does");
    if (context == NULL) {
        return;
    }

    audio.fileData = xudTestAudioFile[5];
    audio.fileBytes = xudTestAudioBytes[5];
    xud_check(xud_load_audio(&handle, context, &audio) == XUD_OK, "gpu-load",
        "the public load decodes the largest of the six files, which is the one load that spends the largest block");
    xud_check(handle.kind == XUD_HANDLE_KIND_AUDIOSTREAM, "gpu-load-kind",
        "and the handle it hands out is tagged AUDIOSTREAM, which is what makes a font handle refused by this close");
    if (handle.kind != XUD_HANDLE_KIND_AUDIOSTREAM) {
        xud_close_context(&context);
        return;
    }

    xud_check(xud_get_audio_info(&info, context, handle) == XUD_OK, "gpu-info", "the public format query answers through a real context");
    xud_check(xud_audio_get(context->handleTable, handle, &record) == XUD_OK, "gpu-record",
        "and the record behind the slot is readable beside it");
    if (record == NULL) {
        xud_close_context(&context);
        return;
    }
    xud_check(info.channels == record->channels && info.sampleRate == record->sampleRate && info.frameCount == record->frameCount,
        "gpu-info-matches-record",
        "and the three answers are the record's own, which is what makes the shell half and this half one decode rather than two hopes");
    xud_check(record->sampleRate > 0u, "gpu-rate", "the rate is answered rather than left at the sentinel");
    xud_check(record->frameCount > 0u, "gpu-frames", "and so is the frame count");
    xud_check(record->frames != NULL, "gpu-block", "and the block exists");

    xud_check(xud_get_audio_length(&seconds, context, handle) == XUD_OK, "gpu-length",
        "the public length answers, and it is the pure divide rather than a length stored at the load");
    xud_check(seconds > 0.0f, "gpu-length-positive", "and answers a positive length of the largest file of the six");

    xud_check(xud_close_audio(context, handle) == XUD_OK, "gpu-close",
        "the public close releases it inside a real context, which is where the block it spent is observable as gone");
    xud_check(xud_audio_close_all(context->handleTable, &closed) == XUD_OK, "gpu-close-all-idempotent",
        "the teardown rung xud_close_context() runs above the table's close runs again with nothing left and is not refused");
    xud_check(closed == 0u, "gpu-close-all-count",
        "and it closed nothing, so the walk is idempotent rather than a double free waiting inside a teardown that will run it again");

    stale = handle;
    stale.generation = handle.generation + 1u;
    xud_check(xud_close_audio(context, stale) == XUD_INVALID_ARG, "gpu-close-stale",
        "a handle kept past the release that freed its slot is refused before anything is freed");
    xud_check(xud_get_audio_info(&info, context, stale) == XUD_INVALID_ARG, "gpu-info-stale", "and so for the format query");
    xud_check(xud_get_audio_length(&seconds, context, stale) == XUD_INVALID_ARG, "gpu-length-stale", "and so for the length query");

    foreign = handle;
    foreign.kind = XUD_HANDLE_KIND_FONT;
    xud_check(xud_close_audio(context, foreign) == XUD_INVALID_ARG, "gpu-close-foreign-kind",
        "and a handle of another kind is refused rather than freeing a record out of that other kind's array");

    xud_check(xud_handle_table_live_count(context->handleTable, XUD_HANDLE_KIND_AUDIOSTREAM, &live) == XUD_OK, "gpu-live-query",
        "the AUDIOSTREAM live count answers after the close");
    xud_check(live == 0u, "gpu-live-zero", "and answers zero, so the context left no stream behind");

    xud_close_context(&context);
    xud_check(context == NULL, "gpu-context-closed",
        "the context is closed and cleared, so the half leaves no block behind - and the AUDIOSTREAM walk ran inside that close above the "
        "table's own");
}

/*-----------------------------------------------------------------------------------------------
 * The play/stop/pause/resume four, against a shell: the four's validation, and the one thing that is answerable
 * without an audio backend
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_audio_play(void) {
    XudContext shell;
    XudContext empty;
    XudHandleTable* table = NULL;
    XudHandle handle = { 0u, 0u, 0u };
    XudHandle stale = { 0u, 0u, 0u };
    XudHandle foreign = { 0u, 0u, 0u };
    XudAudioData audio;
    bool playing = false;

    memset(&shell, 0, sizeof(shell));
    memset(&empty, 0, sizeof(empty));
    memset(&handle, 0, sizeof(handle));
    memset(&audio, 0, sizeof(audio));

    xud_check(xud_handle_table_open(&table) == XUD_OK, "play-table",
        "a bare table is what the pair reaches, which is the point of the pair's validation being answerable with no audio device spent at "
        "all");
    if (table == NULL) {
        return;
    }
    shell.handleTable = table;

    audio.fileData = xudTestAudioFile[0];
    audio.fileBytes = xudTestAudioBytes[0];
    xud_check(
        xud_load_audio(&handle, &shell, &audio) == XUD_OK, "play-loaded", "a stream is loaded to be the thing the pair is asked about");

    xud_check(xud_play_audio(NULL, handle) == XUD_INVALID_ARG, "play-null-context", "a stream with no context to own it is refused");
    xud_check(xud_play_audio(&empty, handle) == XUD_UNSUPPORTED, "play-no-rungs-below",
        "and a context that owns no handle table is refused as UNSUPPORTED rather than as a bad handle, the same split the load above "
        "gives");

    stale = handle;
    stale.generation = handle.generation + 1u;
    xud_check(xud_play_audio(&shell, stale) == XUD_INVALID_ARG, "play-stale",
        "a handle kept past the release that freed its slot is refused as a bad argument, which is the pair's validation running BEFORE "
        "the "
        "sound rung is spent - the one order that makes this answerable at all on a machine with no audio backend");
    foreign = handle;
    foreign.kind = XUD_HANDLE_KIND_MESH;
    xud_check(xud_play_audio(&shell, foreign) == XUD_INVALID_ARG, "play-foreign-kind",
        "and a handle of another kind is refused rather than a row spent at that other kind's slot index");

    xud_check(xud_is_audio_playing(&playing, &shell, handle) == XUD_OK, "play-question-answers",
        "the one question answers through a shell, which is the state this rung owns rather than miniaudio's clock");
    xud_check(playing == false, "play-never-played",
        "and answers false, so a stream that was never played is a stream the mixer is not reading rather than an error the question would "
        "have to guess at");

    xud_check(xud_stop_audio(&shell, handle) == XUD_ALREADY, "play-stop-never-played",
        "a stop on a stream that never had a node answers ALREADY rather than a refusal, which is the state this call asks for already bei"
        "ng the state it has, and it is the state a context that never spent an audio device leaves behind");
    xud_check(xud_stop_audio(&shell, stale) == XUD_INVALID_ARG, "play-stop-stale",
        "and the stop validates the handle even on a context whose sound rung was never spent, which is what makes a stale handle's refus"
        "al a bad argument rather than an ALREADY that would have read the slot");

    xud_check(xud_pause_audio(NULL, handle) == XUD_INVALID_ARG, "pause-null-context",
        "a hold with no context to own it is refused, the same split the play above gives");
    xud_check(xud_pause_audio(&empty, handle) == XUD_UNSUPPORTED, "pause-no-rungs-below",
        "and a context that owns no handle table is refused as UNSUPPORTED rather than as a bad handle, the same split the load above "
        "gives");
    xud_check(xud_pause_audio(&shell, stale) == XUD_INVALID_ARG, "pause-stale",
        "and a handle kept past the release that freed its slot is refused as a bad argument, which is the pair's validation running "
        "BEFORE the sound rung is spent - the one order that makes this answerable at all on a machine with no audio backend");
    xud_check(xud_pause_audio(&shell, foreign) == XUD_INVALID_ARG, "pause-foreign-kind",
        "and a handle of another kind is refused rather than a row spent at that other kind's slot index");
    xud_check(xud_pause_audio(&shell, handle) == XUD_ALREADY, "pause-never-played",
        "a hold on a stream that never had a node answers ALREADY rather than a refusal, which is the state this call asks for already "
        "being the state it has");
    xud_check(xud_resume_audio(&shell, handle) == XUD_ALREADY, "resume-never-paused",
        "and a release of a stream that was never held answers the same, so the pair's two answers are the same state read twice rather "
        "than two different questions");
    xud_check(xud_resume_audio(&shell, stale) == XUD_INVALID_ARG, "resume-stale",
        "and the release validates the handle even on a context whose sound rung was never spent, which is what makes a stale handle's "
        "refusal a bad argument rather than an ALREADY that would have read the slot");

    xud_check(xud_close_audio(&shell, handle) == XUD_OK, "play-close-never-played",
        "the public close releases a stream that was never played, which is the detach above it answering OK on a slot with no row behind i"
        "t");

    xud_check(xud_handle_table_close(&shell.handleTable) == XUD_OK, "play-table-close",
        "and the shell's table closes, so the half leaves no slot and no block behind - and no audio device was ever spent to find any of i"
        "t");
    xud_check(shell.handleTable == NULL, "play-table-cleared", "and is cleared through the shell's own copy");
}

/*-----------------------------------------------------------------------------------------------
 * The volume/pitch/pan triple, against a shell: the defaults, the round-trip, and the refusals that
 * leave the record untouched
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_audio_params(void) {
    XudContext shell;
    XudContext empty;
    XudHandleTable* table = NULL;
    XudHandle handle = { 0u, 0u, 0u };
    XudHandle stale = { 0u, 0u, 0u };
    XudHandle foreign = { 0u, 0u, 0u };
    XudAudioData audio;
    float volume = 0.0f;
    float pitch = 0.0f;
    float pan = 0.0f;

    memset(&shell, 0, sizeof(shell));
    memset(&empty, 0, sizeof(empty));
    memset(&handle, 0, sizeof(handle));
    memset(&audio, 0, sizeof(audio));

    xud_check(xud_handle_table_open(&table) == XUD_OK, "params-table",
        "a bare table is what the pair reaches, which is the point of the validation being answerable with no audio device spent at all");
    if (table == NULL) {
        return;
    }
    shell.handleTable = table;

    audio.fileData = xudTestAudioFile[0];
    audio.fileBytes = xudTestAudioBytes[0];
    xud_check(
        xud_load_audio(&handle, &shell, &audio) == XUD_OK, "params-loaded", "a stream is loaded to be the thing the pair is asked about");

    xud_check(xud_set_audio_volume(NULL, handle, 0.5f) == XUD_INVALID_ARG, "params-set-null-context",
        "a level with no context to own it is refused");
    xud_check(xud_get_audio_volume(NULL, &shell, handle) == XUD_INVALID_ARG, "params-get-null-out",
        "and a level with nowhere to be written is refused");
    xud_check(xud_set_audio_volume(&empty, handle, 0.5f) == XUD_UNSUPPORTED, "params-set-no-rungs-below",
        "and a context that owns no handle table is refused as UNSUPPORTED rather than as a bad handle, the same split the load above "
        "gives");
    xud_check(xud_set_audio_pan(NULL, handle, 0.5f) == XUD_INVALID_ARG, "params-pan-null-context",
        "a position with no context to own it is refused");
    xud_check(xud_get_audio_pan(NULL, &shell, handle) == XUD_INVALID_ARG, "params-pan-null-out",
        "and a position with nowhere to be written is refused beside it");

    xud_check(xud_get_audio_volume(&volume, &shell, handle) == XUD_OK, "params-default-volume-answers",
        "the default answers through a shell, which is the state this rung owns rather than the engine's clock");
    xud_check(volume == 1.0f, "params-default-volume",
        "and it is one, so a stream that has never been given a level plays at the block's own peak rather than at a level the rung chose");
    xud_check(
        xud_get_audio_pitch(&pitch, &shell, handle) == XUD_OK, "params-default-pitch-answers", "and the rate's question answers beside it");
    xud_check(pitch == 1.0f, "params-default-pitch",
        "and it is one, so a stream that has never been given a rate plays at the tempo it was decoded at");
    xud_check(xud_get_audio_pan(&pan, &shell, handle) == XUD_OK, "params-default-pan-answers",
        "and the position's question answers beside the rate");
    xud_check(pan == 0.0f, "params-default-pan",
        "and it is the centre, so a stream that has never been given a position plays centred rather than at a position the rung chose");

    xud_check(xud_set_audio_volume(&shell, handle, -0.1f) == XUD_INVALID_ARG, "params-volume-below-silence",
        "a level below the silence is refused rather than clamped, which is the record answering a value the caller never asked for");
    xud_check(xud_set_audio_volume(&shell, handle, 1.1f) == XUD_INVALID_ARG, "params-volume-above-peak",
        "and a level above the block's own peak is refused the same way, the range the public surface states being the one the setter "
        "holds");
    xud_check(xud_set_audio_pitch(&shell, handle, 0.0f) == XUD_INVALID_ARG, "params-pitch-zero",
        "a rate of zero is refused rather than a no-op, the one the engine would take and leave the row at the rate it had");
    xud_check(
        xud_set_audio_pitch(&shell, handle, -1.0f) == XUD_INVALID_ARG, "params-pitch-negative", "and a negative rate is refused beside it");
    xud_check(xud_set_audio_pan(&shell, handle, -1.1f) == XUD_INVALID_ARG, "params-pan-beyond-left",
        "a position beyond the full left is refused rather than clamped, the engine's own panner being the one that would clamp it");
    xud_check(xud_set_audio_pan(&shell, handle, 1.1f) == XUD_INVALID_ARG, "params-pan-beyond-right",
        "and a position beyond the full right is refused beside it, the range the public surface states being the one the setter holds");

    xud_check(xud_get_audio_volume(&volume, &shell, handle) == XUD_OK, "params-volume-after-refusals",
        "and the level's question answers after the refusals");
    xud_check(volume == 1.0f, "params-volume-untouched",
        "and still answers one, so a refused setter left the record naming the state it had rather than the value it refused");
    xud_check(xud_get_audio_pitch(&pitch, &shell, handle) == XUD_OK, "params-pitch-after-refusals",
        "and the rate's question answers the same way");
    xud_check(pitch == 1.0f, "params-pitch-untouched", "and still answers one, beside the level");
    xud_check(xud_get_audio_pan(&pan, &shell, handle) == XUD_OK, "params-pan-after-refusals",
        "and the position's question answers the same way, after the refusals");
    xud_check(pan == 0.0f, "params-pan-untouched", "and still answers the centre, beside the rate");

    xud_check(xud_set_audio_volume(&shell, handle, 0.25f) == XUD_OK, "params-set-volume",
        "a level inside the range is asked, and the record keeps it where a first play will take it from");
    xud_check(xud_get_audio_volume(&volume, &shell, handle) == XUD_OK, "params-get-volume", "and the question answers beside the set");
    xud_check(volume == 0.25f, "params-volume-roundtrip",
        "and answers the value the setter asked for, so the read-back is the setter's ask rather than a hope about it");

    xud_check(xud_set_audio_pitch(&shell, handle, 1.5f) == XUD_OK, "params-set-pitch",
        "a rate inside its range is asked, and the record keeps it the same way");
    xud_check(xud_get_audio_pitch(&pitch, &shell, handle) == XUD_OK, "params-get-pitch", "and the question answers beside it");
    xud_check(pitch == 1.5f, "params-pitch-roundtrip", "and answers the value the setter asked for, beside the level");
    xud_check(xud_set_audio_pan(&shell, handle, -0.5f) == XUD_OK, "params-set-pan",
        "a position inside the range is asked, and the record keeps it where a first play will take it from, beside the rate");
    xud_check(xud_get_audio_pan(&pan, &shell, handle) == XUD_OK, "params-get-pan", "and the question answers beside the set");
    xud_check(pan == -0.5f, "params-pan-roundtrip", "and answers the value the setter asked for, beside the rate's read-back");

    stale = handle;
    xud_check(xud_close_audio(&shell, handle) == XUD_OK, "params-close",
        "the public close releases the stream, with the level and the rate parked in the record it frees");
    stale.generation = handle.generation + 1u;
    xud_check(xud_set_audio_volume(&shell, stale, 0.5f) == XUD_INVALID_ARG, "params-stale-volume",
        "a handle kept past the close that freed its slot is refused as a bad argument, the accessor running before the record is read");
    xud_check(xud_set_audio_pitch(&shell, stale, 2.0f) == XUD_INVALID_ARG, "params-stale-pitch",
        "and the rate's setter gives the same answer for the same handle, so both read the slot rather than either guessing it");
    foreign = handle;
    foreign.kind = XUD_HANDLE_KIND_MESH;
    xud_check(xud_set_audio_volume(&shell, foreign, 0.5f) == XUD_INVALID_ARG, "params-foreign-kind",
        "and a handle of another kind is refused rather than a record written at that other kind's slot index");
    xud_check(xud_set_audio_pan(&shell, stale, 0.5f) == XUD_INVALID_ARG, "params-stale-pan",
        "and the position's setter gives the same answer for the same handle, beside the rate's");
    xud_check(xud_set_audio_pan(&shell, foreign, 0.5f) == XUD_INVALID_ARG, "params-pan-foreign-kind",
        "and a handle of another kind is refused the same way, beside the level's");

    xud_check(xud_handle_table_close(&shell.handleTable) == XUD_OK, "params-table-close",
        "and the shell's table closes, so the half leaves no slot and no block behind - and no audio device was ever spent to find any of "
        "it");
    xud_check(shell.handleTable == NULL, "params-table-cleared", "and is cleared through the shell's own copy");
}

/*-----------------------------------------------------------------------------------------------
 * The play/stop/pause/resume four, against a real context: the rung spent, and the row's four states
 *-----------------------------------------------------------------------------------------------*/

#define XUD_TEST_AUDIO_END_POLL_LIMIT                                                                                                      \
    500u /* the poll's ceiling: a bound on the wait, not a claim about the stream - five seconds at the tick below */

/* The wait the natural end asks for: the stream runs out on the audio thread, so the only honest wait
   is one that polls the public question until it answers false, with a fixed tick between questions and
   a fixed ceiling on the number of them. The ceiling is where a backend that ran the stream but never
   let it end would be caught, and a question that refused on the way is the same answer.
 */
static bool xud_test_audio_wait_for_end(XudContext* context, XudHandle handle) {
    static const struct timespec tick = { 0, 10000000 }; /* one tick, ten milliseconds */
    bool playing = true;
    uint32_t polls = 0u;

    while ((polls < XUD_TEST_AUDIO_END_POLL_LIMIT) && (playing == true)) {
        nanosleep(&tick, NULL);
        if (xud_is_audio_playing(&playing, context, handle) != XUD_OK) {
            return false;
        }
        polls++;
    }

    return playing == false;
}

/* The wait the duration asks for: the natural end's fixed tick and fixed ceiling, with a clock beside the question, so the end
   is a time the stream's own numbers can be checked against rather than a state the wait merely watched move.
*/
static bool xud_test_audio_wait_for_end_at(XudContext* context, XudHandle handle, double* outSeconds) {
    static const struct timespec tick = { 0, 10000000 }; /* one tick, ten milliseconds */
    bool playing = true;
    uint32_t polls = 0u;

    *outSeconds = 0.0;
    while ((polls < XUD_TEST_AUDIO_END_POLL_LIMIT) && (playing == true)) {
        nanosleep(&tick, NULL);
        if (xud_is_audio_playing(&playing, context, handle) != XUD_OK) {
            return false;
        }
        polls++;
    }

    *outSeconds = (double)polls * 0.01;
    return playing == false;
}

static void xud_test_audio_device(void) {
    XudContext* context = NULL;
    XudHandle handle = { 0u, 0u, 0u };
    XudAudioData audio;
    XudResult played = XUD_INVALID_ARG;
    bool playing = false;
    float volume = 0.0f;
    float pitch = 0.0f;
    float pan = 0.0f;
    uint32_t closed = 0u;
    double endedAt = 0.0;

    memset(&handle, 0, sizeof(handle));
    memset(&audio, 0, sizeof(audio));

    xud_check(xud_init_context(&context, 64u, 64u, "xud_test") == XUD_OK, "device-context",
        "a real context is up, which is the one thing the sound rung needs and a decode does not");
    if (context == NULL) {
        return;
    }

    audio.fileData = xudTestAudioFile[5];
    audio.fileBytes = xudTestAudioBytes[5];
    xud_check(xud_load_audio(&handle, context, &audio) == XUD_OK, "device-loaded",
        "the largest of the files is decoded, which is the stream the row will read rather than a copy of it");

    played = xud_play_audio(context, handle);
    xud_check(played == XUD_OK || played == XUD_UNSUPPORTED, "device-play-answers",
        "the play either spends the rung or refuses: the two answers are this machine's audio backend being there and not being there, an"
        "d neither is a bug - which is why this half is gated rather than counted as a hope");
    if (played != XUD_OK) {
        xud_check(context->sound == NULL, "device-play-refused-spent-nothing",
            "and a refusal leaves the field NULL rather than a rung half-open, which is what makes the teardown below the same on either "
            "answer");
        xud_check(xud_close_audio(context, handle) == XUD_OK, "device-close-after-refusal",
            "and the stream still closes, because a row that was never spent is a row the detach above the close has nothing to refuse");
        xud_check(xud_close_context(&context) == XUD_OK, "device-context-closed",
            "and the context closes over a sound rung that was never opened, which is the ALREADY the eighth name in that teardown gives "
            "for a "
            "field bring-up left NULL");
        xud_check(context == NULL, "device-context-null", "and is cleared");
        return;
    }

    xud_check(context->sound != NULL, "device-rung-spent",
        "the rung is under the context now, which is the lazy spend rather than a bring-up that asked for an audio device nothing was "
        "going to play");
    xud_check(
        xud_is_audio_playing(&playing, context, handle) == XUD_OK, "device-question", "the question answers through the real context");
    xud_check(playing == true, "device-playing",
        "and answers true, so the row's own state is what the question reads rather than a hope about the mixer");

    xud_check(xud_close_audio(context, handle) == XUD_UNSUPPORTED, "device-close-over-playing-row",
        "the public close refuses over a row that is playing, which is the detach above it rather than a stop this rung would have to do "
        "on "
        "the caller's behalf - the one guard that keeps a mixer from reading a block an audio record is about to free");

    xud_check(xud_play_audio(context, handle) == XUD_ALREADY, "device-play-twice",
        "a second play of the same stream answers ALREADY rather than spending a second node, which is the row being the stream's rather "
        "than "
        "raylib's fresh sound per call");

    xud_check(xud_set_audio_volume(context, handle, 0.5f) == XUD_OK, "device-set-volume",
        "a level is asked while the row is playing, which is the write reaching the node the row owns rather than only the record keeping "
        "it");
    xud_check(
        xud_get_audio_volume(&volume, context, handle) == XUD_OK, "device-get-volume", "and the level's question answers beside the set");
    xud_check(volume == 0.5f, "device-volume-roundtrip",
        "and answers the value the setter asked for, so the read-back is the setter's ask with the row live rather than the setter's ask "
        "without one");
    xud_check(xud_set_audio_pitch(context, handle, 2.0f) == XUD_OK, "device-set-pitch",
        "a rate is asked the same way with the row live, which is the write the row's sound was built to take rather than one a flag would "
        "have made a no-op");
    xud_check(xud_get_audio_pitch(&pitch, context, handle) == XUD_OK, "device-get-pitch", "and the rate's question answers beside it");
    xud_check(pitch == 2.0f, "device-pitch-roundtrip", "and answers the value the setter asked for, beside the level");
    xud_check(xud_set_audio_volume(context, handle, 1.1f) == XUD_INVALID_ARG, "device-volume-refused-with-row",
        "a level outside the range is refused with the row live too, so the refusal is the setter's own rather than the row's absence");
    xud_check(xud_set_audio_pitch(context, handle, 0.0f) == XUD_INVALID_ARG, "device-pitch-refused-with-row",
        "and a non-positive rate is refused the same way, beside the level");
    xud_check(xud_set_audio_pan(context, handle, -0.5f) == XUD_OK, "device-set-pan",
        "a position is asked the same way with the row live, which is the write the row's panner takes rather than only the record keeping "
        "it");
    xud_check(xud_get_audio_pan(&pan, context, handle) == XUD_OK, "device-get-pan", "and the position's question answers beside it");
    xud_check(pan == -0.5f, "device-pan-roundtrip", "and answers the value the setter asked for, beside the rate");
    xud_check(xud_set_audio_pan(context, handle, 1.1f) == XUD_INVALID_ARG, "device-pan-refused-with-row",
        "and a position beyond the range is refused with the row live too, beside the level's");

    xud_check(xud_stop_audio(context, handle) == XUD_OK, "device-stop", "the stop tells the mixer, and the node stays attached");
    xud_check(xud_stop_audio(context, handle) == XUD_ALREADY, "device-stop-twice",
        "and a stop run twice answers ALREADY rather than a refusal, which is the pair being idempotent rather than a second driver call "
        "wait"
        "ing behind it");
    xud_check(
        xud_is_audio_playing(&playing, context, handle) == XUD_OK, "device-question-after-stop", "the question answers beside the stop");
    xud_check(playing == false, "device-stopped",
        "and answers false, so the row is a node that exists and is not reading rather than a node that was destroyed");

    xud_check(xud_play_audio(context, handle) == XUD_OK, "device-play-resumes",
        "a play after a stop answers OK, which is the node staying attached: the row went back to the next play of the same slot rather "
        "than "
        "to a new one");
    xud_check(xud_stop_audio(context, handle) == XUD_OK, "device-stop-before-close",
        "and the caller stops it before closing, which is the answer the refusal above asked for");

    xud_check(xud_close_audio(context, handle) == XUD_OK, "device-close",
        "the public close releases it, with the detach above it having given the bufferRef reference back before the record's block went");

    /* The natural end, on the shortest stream: the mixer runs it out on its own thread, so the row's
       state is the one thing this half can watch move between the two clocks. */
    audio.fileData = xudTestAudioFile[0];
    audio.fileBytes = xudTestAudioBytes[0];
    xud_check(xud_load_audio(&handle, context, &audio) == XUD_OK, "device-natural-loaded",
        "the shortest of the files is decoded, which is the stream the natural end is watched on rather than the minutes the largest one "
        "would wait");
    played = xud_play_audio(context, handle);
    xud_check(played == XUD_OK, "device-natural-play",
        "the play spends the same rung for a second slot, which is a new row beside the engine rather than a second engine");
    xud_check(xud_is_audio_playing(&playing, context, handle) == XUD_OK, "device-natural-question", "the question answers beside the play");
    xud_check(playing == true, "device-natural-playing", "and answers true, which is the state the wait below has to watch move");
    xud_check(xud_test_audio_wait_for_end(context, handle), "device-natural-ended",
        "polled until the question answers false: the stream ran out on the audio thread, and the row's state is settled to stopped by the "
        "question rather than left naming a mix that no longer happens");
    played = xud_play_audio(context, handle);
    xud_check(played == XUD_OK, "device-natural-replay",
        "and a play on a row that reached the end answers OK rather than ALREADY, which is the restart the play half states: the row "
        "settles to stopped, and ma_sound_start seeks back to frame 0");
    xud_check(xud_is_audio_playing(&playing, context, handle) == XUD_OK, "device-natural-replay-question",
        "the question answers beside the restart");
    xud_check(playing == true, "device-natural-replaying", "and answers true again, so the restart is a mix rather than a state flip");
    xud_check(xud_test_audio_wait_for_end(context, handle), "device-natural-ended-again",
        "and the second run out settles the same way, so the settle is the row's standing behavior rather than a one-time move");
    xud_check(xud_close_audio(context, handle) == XUD_OK, "device-natural-close",
        "and the close answers OK over a row that reached its end: the detach's refusal is a row the mixer is reading, and a stream at its "
        "end is a stream it is not");

    /* The duration, on the two-second take: the stream's own numbers say when the walk is over, and a rate
       the engine's device does not hold is the one the resampler has to answer for. */
    audio.fileData = xudTestAudioFile[XUD_TEST_AUDIO_OGG_INDEX];
    audio.fileBytes = xudTestAudioBytes[XUD_TEST_AUDIO_OGG_INDEX];
    xud_check(xud_load_audio(&handle, context, &audio) == XUD_OK, "device-duration-loaded",
        "the two-second take is decoded, which is the stream the duration is timed on rather than a state the wait below merely watched "
        "move");
    played = xud_play_audio(context, handle);
    xud_check(played == XUD_OK, "device-duration-play", "the play spends the same rung for the take");
    xud_check(
        xud_is_audio_playing(&playing, context, handle) == XUD_OK, "device-duration-question", "the question answers beside the play");
    xud_check(playing == true, "device-duration-playing", "and answers true, which is the state the clock below has to watch");
    xud_check(xud_test_audio_wait_for_end_at(context, handle, &endedAt), "device-duration-ended",
        "polled with a clock until the question answers false: the take ran out on the audio thread, and the clock is the time the end "
        "landed on");
    xud_check((endedAt > 1.5) && (endedAt < 3.0), "device-duration-window",
        "and the end is a time on the take's own numbers, within half a second of its length on either side: a walk timed at the device's "
        "rate rather than the take's is the one the window catches, in both directions");
    xud_check(
        xud_close_audio(context, handle) == XUD_OK, "device-duration-close", "and the close answers OK over a row that reached its end");

    /* Pause and resume, on the two-second take: the hold is the one state the natural end above cannot set, so the take is the
       stream the hold is watched on, and the clock is the one thing that can say the resume took the cursor from the hold rather
       than from frame 0. */
    audio.fileData = xudTestAudioFile[XUD_TEST_AUDIO_OGG_INDEX];
    audio.fileBytes = xudTestAudioBytes[XUD_TEST_AUDIO_OGG_INDEX];
    xud_check(xud_load_audio(&handle, context, &audio) == XUD_OK, "device-pause-loaded",
        "the two-second take is decoded again, in a slot of its own, which is the stream the hold is watched on rather than a state "
        "the wait below merely watched move");
    played = xud_play_audio(context, handle);
    xud_check(played == XUD_OK, "device-pause-play", "the play spends the same rung for the take");
    xud_check(xud_is_audio_playing(&playing, context, handle) == XUD_OK, "device-pause-question", "the question answers beside the play");
    xud_check(playing == true, "device-pause-playing", "and answers true, which is the state the hold below has to take");

    /* The wait for the hold to have a middle of the stream to hold: a fixed tick until the clock is half the take's length, so the
       resume below has a cursor in the block rather than at frame 0 or at the end. */
    {
        static const struct timespec tick = { 0, 10000000 }; /* one tick, ten milliseconds */
        double elapsed = 0.0;
        while ((elapsed < 0.5) && (playing == true)) {
            nanosleep(&tick, NULL);
            if (xud_is_audio_playing(&playing, context, handle) != XUD_OK) {
                playing = false;
            }
            elapsed += 0.01;
        }
    }
    xud_check(playing == true, "device-pause-still-playing",
        "the take is still reading at the half-second mark, so the hold below is a hold on the middle of the stream rather than on "
        "its end");

    xud_check(xud_pause_audio(context, handle) == XUD_OK, "device-pause",
        "the hold answers OK on a reading row: the node stays attached and the cursor stays where the node left it");
    xud_check(
        xud_is_audio_playing(&playing, context, handle) == XUD_OK, "device-pause-question-again", "the question answers beside the hold");
    xud_check(playing == false, "device-paused",
        "and answers false: the mixer is not reading the held row, which is the hold's own difference from the stop above, whose row "
        "says stopped rather than held");
    xud_check(xud_pause_audio(context, handle) == XUD_ALREADY, "device-pause-twice",
        "and a hold run twice is not a hold that refused the second time, which is the state this call asks for already being the "
        "state it has");

    xud_check(xud_resume_audio(context, handle) == XUD_OK, "device-resume",
        "and the release answers OK on the held row: the node goes back to the mix with the cursor where the hold left it");
    xud_check(
        xud_is_audio_playing(&playing, context, handle) == XUD_OK, "device-resume-question", "the question answers beside the release");
    xud_check(playing == true, "device-resumed", "and answers true again, so the release is a mix rather than a state flip");
    xud_check(xud_resume_audio(context, handle) == XUD_ALREADY, "device-resume-twice",
        "and a release run twice answers ALREADY on a row that is playing for real");
    xud_check(xud_test_audio_wait_for_end_at(context, handle, &endedAt), "device-pause-ended",
        "polled with a clock until the question answers false: the take ran out on the audio thread after the release");
    xud_check((endedAt > 1.0) && (endedAt < 1.8), "device-resume-continues",
        "and the end is a time on the take's own numbers, not on the take's full length: a release that had taken the cursor back to "
        "frame 0 would land near the whole two seconds, and one that had taken it past the hold would land near zero, so the window "
        "catches both the restart and the skip");

    /* The stop on the hold, and the close over it: the held row is the one state the stop's own check does not move, so the pair is
       watched on a row of its own. */
    played = xud_play_audio(context, handle);
    xud_check(played == XUD_OK, "device-pause-replay",
        "a play on a row that reached its end answers OK, the restart the play half states, which puts the row back to reading for "
        "the hold below");
    xud_check(xud_is_audio_playing(&playing, context, handle) == XUD_OK, "device-pause-replay-question",
        "the question answers beside the restart");
    xud_check(playing == true, "device-pause-replaying", "and answers true, which is the state the hold below has to take");
    xud_check(xud_pause_audio(context, handle) == XUD_OK, "device-pause-hold",
        "and the hold takes the row off the mix again, so the stop below has a held row rather than a reading one");
    xud_check(xud_stop_audio(context, handle) == XUD_ALREADY, "device-stop-held",
        "and the stop on the held row answers ALREADY rather than OK: the state it asks for - the mixer not reading the row - already "
        "holds, and the row stays held for the close below rather than moving to stopped");
    xud_check(
        xud_is_audio_playing(&playing, context, handle) == XUD_OK, "device-stop-held-question", "the question answers beside the refusal");
    xud_check(playing == false, "device-stop-held-still-paused",
        "and answers false, so the refusal moved nothing: the row's state is the hold's rather than the stop's");
    xud_check(xud_close_audio(context, handle) == XUD_OK, "device-close-held",
        "and the close answers OK over a held row: the detach's refusal is a row the mixer is reading, and a held row is a row it is "
        "not");

    xud_check(xud_sound_close(&context->sound, &closed) == XUD_OK, "device-sound-close",
        "the rung's own teardown closes what the play spent, and the context's teardown runs the same rung above the audio's close_all");
    xud_check(context->sound == NULL, "device-sound-cleared",
        "and is cleared through the context's own copy of the pointer, which is what makes the teardown below not run it twice");
    xud_check(
        xud_close_context(&context) == XUD_OK, "device-context-closed", "the context closes over a sound rung that was already closed");
    xud_check(context == NULL, "device-context-null", "and is cleared, so the half leaves no row, no engine and no block behind");
}

/*-----------------------------------------------------------------------------------------------
 * The positional half, headless: the rolloff and the pan as arithmetic, with no device and no file
 *
 *   Every number below is stated against the formula the public header states rather than against a
 *   table of values someone read off a run: the distance is the one the check itself picks, the
 *   rolloff is that distance divided into the base, and the pan is the dot the derived right gives.
 *   The listener is the origin facing +x for the pair that needs a left and a right, because that
 *   facing's derived right IS +z - the axis the two sources sit on - so the expected pan is the axis
 *   the source was placed on rather than a vector the reader has to derive to follow the check.
 *-----------------------------------------------------------------------------------------------*/

/* The one comparison the spatial numbers are read with: the pair is float arithmetic on sqrtf, and a
   check that demanded bit equality would be a check on the rounding of one machine's sqrtf.
*/
static int xud_test_audio_close(float a, float b) {
    float delta = a - b;
    return ((delta > -0.0001f) && (delta < 0.0001f)) ? 1 : 0;
}

static void xud_test_audio_spatial(void) {
    XudAudioListener listener = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } };
    XudVector3 source = { 0.0f, 0.0f, 0.0f };
    float volume = 0.0f;
    float pan = 0.0f;

    /* Ahead at five: the distance divides the base, and the facing has nothing to say. */
    source.x = 5.0f;
    xud_check(xud_audio_spatial_gain(&volume, &pan, &listener, &source, 1.0f, 0.0f) == XUD_OK, "spatial-ahead-answers",
        "a source straight ahead of the ear is answerable, which is the arithmetic running with no device under it at all");
    xud_check(xud_test_audio_close(volume, 0.2f), "spatial-ahead-rolloff",
        "and the level is the base divided by the five the check placed it at - the linear rolloff, not a square of it and not a plateau");
    xud_check(xud_test_audio_close(pan, 0.0f), "spatial-ahead-centred",
        "and the position is the centre, because straight ahead is neither left nor right of the facing");

    /* The right and the left of the same distance: the derived right is +z for this facing, so a
       source on +z is the full right and one on -z the full left, at the SAME distance and so the
       same level - the pair being about direction alone.
    */
    source.x = 0.0f;
    source.z = 5.0f;
    xud_check(xud_audio_spatial_gain(&volume, &pan, &listener, &source, 1.0f, 0.0f) == XUD_OK, "spatial-right-answers",
        "a source on the ear's right is answerable");
    xud_check(xud_test_audio_close(pan, 1.0f), "spatial-right-pan",
        "and answers the full right, which is the dot of the direction and the derived right - right being cross(forward, {0,1,0}) and "
        "this "
        "facing's right being +z");
    xud_check(xud_test_audio_close(volume, 0.2f), "spatial-right-same-level",
        "and the same level the front source answered, so the pan is a direction and not a second attenuation");
    source.z = -5.0f;
    xud_check(xud_audio_spatial_gain(&volume, &pan, &listener, &source, 1.0f, 0.0f) == XUD_OK, "spatial-left-answers",
        "and the mirror source on the ear's left is answerable beside it");
    xud_check(xud_test_audio_close(pan, -1.0f), "spatial-left-pan",
        "and answers the full LEFT, the sign the check is here for: the pan flips with the axis, not with the distance");
    xud_check(xud_test_audio_close(volume, 0.2f), "spatial-left-same-level", "and the same level again, beside the right's");

    /* Behind at the same five: the facing has something to say, and what it says is quieter. */
    source.x = -5.0f;
    source.z = 0.0f;
    xud_check(xud_audio_spatial_gain(&volume, &pan, &listener, &source, 1.0f, 0.0f) == XUD_OK, "spatial-behind-answers",
        "a source behind the ear is answerable, the facing being read rather than the distance alone");
    xud_check(xud_test_audio_close(volume, 0.1f), "spatial-behind-quieter",
        "and half the level the same distance in front answered: the dot is -1, so the front factor of (1 + 0.5*dot) is a half - the half "
        "of "
        "raylib's model that reads which way the ear looks");
    xud_check(xud_test_audio_close(pan, 0.0f), "spatial-behind-centred",
        "and still the centre, because straight behind is on the axis the right is measured along by zero");

    /* At the ear itself: no distance to roll off over and no direction to pan by. */
    source.x = 0.0f;
    xud_check(xud_audio_spatial_gain(&volume, &pan, &listener, &source, 1.0f, 0.0f) == XUD_OK, "spatial-at-ear-answers",
        "a source AT the listener is answerable rather than a divide by zero");
    xud_check(volume == 1.0f, "spatial-at-ear-base",
        "and the level is the base itself, the plateau at and inside a unit's distance being the clamp that keeps d = 0 the base rather "
        "than "
        "an infinity the node's own range refuses");
    xud_check(pan == 0.0f, "spatial-at-ear-centred",
        "and the position is the base position, a source at the ear having no direction to be panned by");

    /* The plateau and the clamp, on a base below the peak: inside the unit the base is what answers,
       and a base pan biased toward one side cannot be pushed past the range by the geometry.
    */
    source.x = 0.25f;
    xud_check(xud_audio_spatial_gain(&volume, &pan, &listener, &source, 0.5f, 0.0f) == XUD_OK, "spatial-near-answers",
        "a source a quarter of the way to a unit is answerable");
    xud_check(xud_test_audio_close(volume, 0.5f), "spatial-near-plateau",
        "and answers its base rather than the double a bare divide would have answered: the rolloff starts at the unit, so nothing is "
        "louder "
        "for being closer than the unit it is already inside");
    source.x = 0.0f;
    source.z = 1.0f;
    xud_check(xud_audio_spatial_gain(&volume, &pan, &listener, &source, 0.5f, 0.75f) == XUD_OK, "spatial-biased-answers",
        "and a source to the right of a base position already biased to the right is answerable");
    xud_check(pan == 1.0f, "spatial-biased-clamped",
        "and the sum is CLAMPED to the full right rather than refused or wrapped - the spatial pair is computed rather than asked for, so "
        "the "
        "range's edge is the answer a caller could not have avoided");

    /* A non-unit facing answers the unit one's pair: the helper normalises before it derives. */
    listener.forward.x = 3.0f;
    source.x = 5.0f;
    source.z = 0.0f;
    xud_check(xud_audio_spatial_gain(&volume, &pan, &listener, &source, 1.0f, 0.0f) == XUD_OK, "spatial-long-facing-answers",
        "a facing handed over three times the length it needs is answerable");
    xud_check(xud_test_audio_close(volume, 0.2f) && xud_test_audio_close(pan, 0.0f), "spatial-long-facing-normalised",
        "and answers the pair the unit facing answered at the same place, so the arithmetic reads a direction rather than a length");

    /* The refusals: the caller's shapes and the world's one bad facing. */
    listener.forward.x = 1.0f;
    xud_check(xud_audio_spatial_gain(NULL, &pan, &listener, &source, 1.0f, 0.0f) == XUD_INVALID_ARG, "spatial-null-out-volume",
        "a level with nowhere to be written is refused");
    xud_check(xud_audio_spatial_gain(&volume, NULL, &listener, &source, 1.0f, 0.0f) == XUD_INVALID_ARG, "spatial-null-out-pan",
        "and a position with nowhere to be written beside it");
    xud_check(xud_audio_spatial_gain(&volume, &pan, NULL, &source, 1.0f, 0.0f) == XUD_INVALID_ARG, "spatial-null-listener",
        "and a gain with no ear to measure from is refused");
    source.x = (float)0.0 / (float)0.0;
    xud_check(xud_audio_spatial_gain(&volume, &pan, &listener, &source, 1.0f, 0.0f) == XUD_INVALID_ARG, "spatial-source-nan",
        "and a source that is not a number is refused rather than answered with a NaN a node would then play");
    source.x = 5.0f;
    xud_check(xud_audio_spatial_gain(&volume, &pan, &listener, &source, (float)0.0 / (float)0.0, 0.0f) == XUD_INVALID_ARG,
        "spatial-base-nan", "and a base level that is not a number is refused beside the source that is not one");
    xud_check(xud_audio_spatial_gain(&volume, &pan, &listener, &source, 1.0f, (float)0.0 / (float)0.0) == XUD_INVALID_ARG,
        "spatial-base-pan-nan", "and a base position that is not a number is refused with it, the two bases being the same kind of ask");
    xud_check(xud_audio_spatial_gain(&volume, &pan, &listener, &source, 1.0f, 0.0f) == XUD_OK, "spatial-finite-answers",
        "and the finite pair answers again after it");
    listener.forward.x = 0.0f;
    listener.forward.y = 1.0f;
    xud_check(xud_audio_spatial_gain(&volume, &pan, &listener, &source, 1.0f, 0.0f) == XUD_UNSUPPORTED, "spatial-facing-up-refused",
        "a facing straight up the world's own axis is refused: cross(forward, up) is the zero vector, which is a facing with no left or "
        "right "
        "to pan against");
    listener.forward.y = 0.0f;
    xud_check(xud_audio_spatial_gain(&volume, &pan, &listener, &source, 1.0f, 0.0f) == XUD_UNSUPPORTED, "spatial-facing-zero-refused",
        "and a facing of no length is refused the same way, the zero forward being the same shape with no direction attached");
}

/*-----------------------------------------------------------------------------------------------
 * The positional pair against a shell: the place, the base that survives it, and the listener that
 * has no device to live in yet
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_audio_spatial_shell(void) {
    XudContext shell;
    XudContext empty;
    XudHandleTable* table = NULL;
    XudHandle handle = { 0u, 0u, 0u };
    XudHandle stale = { 0u, 0u, 0u };
    XudHandle foreign = { 0u, 0u, 0u };
    XudAudioData audio;
    XudVector3 place = { 1.0f, 2.0f, 3.0f };
    XudVector3 notANumber = { (float)0.0 / (float)0.0, 0.0f, 0.0f };
    XudVector3 read = { 0.0f, 0.0f, 0.0f };
    float volume = 0.0f;
    float pan = 0.0f;

    memset(&shell, 0, sizeof(shell));
    memset(&empty, 0, sizeof(empty));
    memset(&audio, 0, sizeof(audio));

    xud_check(xud_handle_table_open(&table) == XUD_OK, "spatial-shell-table",
        "a bare table is what the pair reaches, which is why the place and the base are checkable with no audio device spent at all");
    if (table == NULL) {
        return;
    }
    shell.handleTable = table;

    audio.fileData = xudTestAudioFile[0];
    audio.fileBytes = xudTestAudioBytes[0];
    xud_check(xud_load_audio(&handle, &shell, &audio) == XUD_OK, "spatial-shell-loaded",
        "a stream is loaded to be the thing the pair is asked about");

    xud_check(xud_get_audio_source_position(&read, &shell, handle) == XUD_OK, "spatial-shell-origin-answers",
        "the source's question answers before any place, through a shell and with no device under it");
    xud_check((read.x == 0.0f) && (read.y == 0.0f) && (read.z == 0.0f), "spatial-shell-origin",
        "and answers the world origin, which is the state the header states beside the non-spatial one rather than a flag asked about "
        "separately");

    xud_check(xud_set_audio_source_position(NULL, handle, place) == XUD_INVALID_ARG, "spatial-shell-null-context",
        "a position with no context to own it is refused");
    xud_check(xud_get_audio_source_position(NULL, &shell, handle) == XUD_INVALID_ARG, "spatial-shell-null-out",
        "and a position with nowhere to be written is refused beside it");
    xud_check(xud_set_audio_source_position(&empty, handle, place) == XUD_UNSUPPORTED, "spatial-shell-no-table",
        "and a context that owns no handle table is refused as UNSUPPORTED rather than as a bad handle, the same split the level's setter "
        "gives");
    xud_check(xud_set_audio_source_position(&shell, handle, notANumber) == XUD_INVALID_ARG, "spatial-shell-source-nan",
        "a position that is not a number is refused rather than stored, the arithmetic it would feed being the one that answers NaN for "
        "it");

    xud_check(xud_set_audio_source_position(&shell, handle, place) == XUD_OK, "spatial-shell-place",
        "a fixed world position is asked, and the handle is SPATIAL from this line on");
    xud_check(xud_get_audio_source_position(&read, &shell, handle) == XUD_OK, "spatial-shell-read-answers",
        "and the source's question answers beside the place");
    xud_check((read.x == 1.0f) && (read.y == 2.0f) && (read.z == 3.0f), "spatial-shell-position-roundtrip",
        "and answers the three numbers the setter asked for, so the read-back is the place rather than a hope about it");

    xud_check(xud_set_audio_volume(&shell, handle, 0.5f) == XUD_OK, "spatial-shell-base-volume-asked",
        "a level is asked of a handle that is already placed, which is the record keeping a BASE rather than a level a mixer would hear");
    xud_check(xud_get_audio_volume(&volume, &shell, handle) == XUD_OK, "spatial-shell-volume-answers", "and the level's question answers");
    xud_check(volume == 0.5f, "spatial-shell-volume-is-base",
        "and answers the caller's own 0.5 rather than the value the distance to the default listener would make of it - the choice the "
        "public header states, pinned here so a read-back that started answering the computed number fails this area");
    xud_check(xud_set_audio_pan(&shell, handle, -0.5f) == XUD_OK, "spatial-shell-base-pan-asked",
        "a position is asked the same way, beside the level");
    xud_check(xud_get_audio_pan(&pan, &shell, handle) == XUD_OK, "spatial-shell-pan-answers", "and the position's question answers");
    xud_check(pan == -0.5f, "spatial-shell-pan-is-base",
        "and answers the caller's own -0.5 rather than the dot the geometry would produce, the base being the state both getters answer");

    xud_check(xud_set_audio_listener_position(NULL, place) == XUD_INVALID_ARG, "spatial-shell-listener-null-context",
        "an ear with no context to own it is refused, the same first guard every call above gives");
    xud_check(xud_get_audio_listener_position(NULL, &shell) == XUD_INVALID_ARG, "spatial-shell-listener-null-out",
        "and an ear with nowhere to be written is refused beside it");
    xud_check(xud_get_audio_listener_forward(NULL, &shell) == XUD_INVALID_ARG, "spatial-shell-forward-null-out",
        "and a facing with nowhere to be written is refused the same way");
    xud_check(xud_set_audio_listener_position(&empty, place) == XUD_UNSUPPORTED, "spatial-shell-listener-no-table",
        "and a context that owns no handle table is refused as UNSUPPORTED, the bring-up guard running before the device's absence is "
        "asked "
        "about");
    xud_check(xud_set_audio_listener_position(&shell, place) == XUD_UNSUPPORTED, "spatial-shell-listener-no-device",
        "and a context that never played anything is refused as UNSUPPORTED too: the listener is the audio device's own state, and a "
        "context "
        "with no device has no ear to move - which is the one place this pair refuses a shape the level's setters take and store");
    xud_check(xud_set_audio_listener_forward(&shell, place) == XUD_UNSUPPORTED, "spatial-shell-forward-no-device",
        "and the facing is refused for the same reason beside the position");
    xud_check(xud_get_audio_listener_position(&read, &shell) == XUD_UNSUPPORTED, "spatial-shell-listener-read-no-device",
        "and the ear's question answers UNSUPPORTED rather than the default it would have, because there is no device to answer from");
    xud_check(xud_get_audio_listener_forward(&read, &shell) == XUD_UNSUPPORTED, "spatial-shell-forward-read-no-device",
        "and the facing's question answers the same way beside it");

    xud_check(xud_get_audio_source_position(&read, &shell, handle) == XUD_OK, "spatial-shell-position-still-answers",
        "the source's question still answers after all of that");
    xud_check((read.x == 1.0f) && (read.y == 2.0f) && (read.z == 3.0f), "spatial-shell-position-untouched",
        "and still the placed three, so the refused listener calls left the record naming the place it had");

    stale = handle;
    stale.generation = handle.generation + 1u;
    xud_check(xud_set_audio_source_position(&shell, stale, place) == XUD_INVALID_ARG, "spatial-shell-stale",
        "a handle kept past the close that freed its slot is refused as a bad argument, the accessor running before a position is stored");
    foreign = handle;
    foreign.kind = XUD_HANDLE_KIND_MESH;
    xud_check(xud_set_audio_source_position(&shell, foreign, place) == XUD_INVALID_ARG, "spatial-shell-foreign-kind",
        "and a handle of another kind is refused rather than a record of that other kind written a position into");
    xud_check(xud_get_audio_source_position(&read, &shell, stale) == XUD_INVALID_ARG, "spatial-shell-read-stale",
        "and the source's question gives the same answer for the same stale handle");

    xud_check(xud_close_audio(&shell, handle) == XUD_OK, "spatial-shell-close",
        "the public close releases the placed stream, with its position and its two bases parked in the record it frees");
    xud_check(xud_handle_table_close(&shell.handleTable) == XUD_OK, "spatial-shell-table-close",
        "and the shell's table closes, so the half leaves no slot and no block behind - and no audio device was ever spent to find any of "
        "it");
    xud_check(shell.handleTable == NULL, "spatial-shell-table-cleared", "and is cleared through the shell's own copy");
}

/*-----------------------------------------------------------------------------------------------
 * The positional pair against a real context: the row hears the geometry, the getters answer the
 * base, and one call that moves the ear moves every placed row
 *-----------------------------------------------------------------------------------------------*/

/* What the node itself was told: the row is this rung's bookkeeping and the node is what the mixer
   reads, so the one claim this half can make about a SPATIAL handle - that the node hears the value
   the geometry produced rather than the base the record keeps - is read off the node, through the
   row the device owns at the stream's own slot index.
*/
static int xud_test_audio_node(float* outVolume, float* outPan, XudContext* context, XudHandle handle) {
    XudSoundRow* row = NULL;

    if ((context == NULL) || (context->sound == NULL) || (context->sound->rows == NULL) || (handle.index >= context->sound->rowCount)) {
        return 0;
    }
    row = context->sound->rows[handle.index];
    if (row == NULL) {
        return 0;
    }

    *outVolume = ma_sound_get_volume(&row->sound);
    *outPan = ma_sound_get_pan(&row->sound);
    return 1;
}

static void xud_test_audio_spatial_device(void) {
    XudContext* context = NULL;
    XudHandle placed = { 0u, 0u, 0u };
    XudHandle plain = { 0u, 0u, 0u };
    XudAudioData audio;
    XudVector3 place = { -5.0f, 0.0f, 0.0f };
    XudVector3 zero = { 0.0f, 0.0f, 0.0f };
    XudVector3 up = { 0.0f, 1.0f, 0.0f };
    XudVector3 longForward = { 0.0f, 0.0f, 2.0f };
    XudVector3 notANumber = { (float)0.0 / (float)0.0, 0.0f, 0.0f };
    XudVector3 read = { 0.0f, 0.0f, 0.0f };
    float volume = 0.0f;
    float pan = 0.0f;
    float nodeVolume = 0.0f;
    float nodePan = 0.0f;
    uint32_t closed = 0u;
    XudResult played = XUD_INVALID_ARG;

    memset(&placed, 0, sizeof(placed));
    memset(&plain, 0, sizeof(plain));
    memset(&audio, 0, sizeof(audio));

    xud_check(xud_init_context(&context, 64u, 64u, "xud_test") == XUD_OK, "spatial-device-context",
        "a real context is up, which is the one thing the listener needs and the arithmetic above did not");
    if (context == NULL) {
        return;
    }

    audio.fileData = xudTestAudioFile[0];
    audio.fileBytes = xudTestAudioBytes[0];
    xud_check(xud_load_audio(&placed, context, &audio) == XUD_OK, "spatial-device-loaded",
        "the shortest of the files is decoded, which is the stream the place is stated on");

    played = xud_play_audio(context, placed);
    xud_check(played == XUD_OK || played == XUD_UNSUPPORTED, "spatial-device-play-answers",
        "the play either spends the rung or refuses, and neither answer is a bug - the same gate the device half above accepts");
    if (played != XUD_OK) {
        xud_check(xud_close_audio(context, placed) == XUD_OK, "spatial-device-close-after-refusal",
            "and the stream still closes over a rung that was never spent");
        xud_check(xud_close_context(&context) == XUD_OK, "spatial-device-context-closed", "and the context closes with it");
        return;
    }

    xud_check(xud_get_audio_listener_position(&read, context) == XUD_OK, "spatial-device-listener-answers",
        "the ear's question answers once a device is under the context, which is the state the shell refused for");
    xud_check((read.x == 0.0f) && (read.y == 0.0f) && (read.z == 0.0f), "spatial-device-listener-default",
        "and answers the world origin, a device that was never moved being where the world starts");
    xud_check(xud_get_audio_listener_forward(&read, context) == XUD_OK, "spatial-device-forward-answers",
        "and the facing's question answers beside it");
    xud_check((read.x == 0.0f) && (read.y == 0.0f) && (read.z == 1.0f), "spatial-device-forward-default",
        "and answers +z, the facing xud_sound_open() writes rather than the calloc's zero the derived right would have refused");
    xud_check(xud_set_audio_listener_forward(context, zero) == XUD_INVALID_ARG, "spatial-device-forward-zero-refused",
        "a facing of no length is refused rather than stored, the derived right being the divide it would have fed");
    xud_check(xud_set_audio_listener_forward(context, up) == XUD_INVALID_ARG, "spatial-device-forward-up-refused",
        "and a facing straight up the world is refused beside it: cross(forward, {0,1,0}) is the zero vector, which is a facing with no "
        "left "
        "or right to pan against");
    xud_check(xud_set_audio_listener_position(context, notANumber) == XUD_INVALID_ARG, "spatial-device-listener-nan-refused",
        "and an ear that is not a number is refused once there IS a device to store it in, the shell having refused for the missing device "
        "first");
    xud_check(xud_get_audio_listener_forward(&read, context) == XUD_OK, "spatial-device-forward-still-answers",
        "the facing's question answers after the three refusals");
    xud_check((read.x == 0.0f) && (read.y == 0.0f) && (read.z == 1.0f), "spatial-device-forward-untouched",
        "and still +z, so a refused turn left the ear looking where it did");
    xud_check(xud_set_audio_listener_forward(context, longForward) == XUD_OK, "spatial-device-forward-long-asked",
        "a facing handed over twice the length it needs is taken");
    xud_check(xud_get_audio_listener_forward(&read, context) == XUD_OK, "spatial-device-forward-normalised-answers",
        "and the question answers beside it");
    xud_check((read.x == 0.0f) && (read.y == 0.0f) && (read.z == 1.0f), "spatial-device-forward-normalised",
        "and answers the UNIT, the setter storing the direction rather than the caller's twice-long bytes - the one read-back in this pair "
        "that does not answer the caller's own numbers");

    /* The non-spatial handle, and the ear moving under it: the level the caller asked is the level the
       node hears, before and after a listener move, which is the whole of the promise that a handle
       nobody placed is untouched by any of this.
    */
    audio.fileData = xudTestAudioFile[2];
    audio.fileBytes = xudTestAudioBytes[2];
    xud_check(xud_load_audio(&plain, context, &audio) == XUD_OK, "spatial-device-plain-loaded",
        "a second stream is decoded in a slot of its own and left where it is: never placed, so never spatial");
    played = xud_play_audio(context, plain);
    xud_check(played == XUD_OK, "spatial-device-plain-play", "and the play spends a row for it beside the first row");
    xud_check(xud_set_audio_volume(context, plain, 0.5f) == XUD_OK, "spatial-device-plain-volume",
        "a level is asked of the handle that was never placed");
    xud_check(
        xud_set_audio_pan(context, plain, -0.5f) == XUD_OK, "spatial-device-plain-pan", "and a position is asked of it beside the level");
    xud_check(xud_test_audio_node(&nodeVolume, &nodePan, context, plain) == 1, "spatial-device-plain-row",
        "the plain handle's row is live and answerable");
    xud_check(xud_test_audio_close(nodeVolume, 0.5f) && xud_test_audio_close(nodePan, -0.5f), "spatial-device-plain-node-is-ask",
        "and the node hears the two values the caller asked for, which is the non-spatial story told unchanged: no rolloff, no pan from a "
        "facing, no geometry at all");
    xud_check(xud_set_audio_listener_position(context, place) == XUD_OK, "spatial-device-plain-listener-moved",
        "the ear is moved under both handles");
    xud_check(xud_test_audio_node(&nodeVolume, &nodePan, context, plain) == 1, "spatial-device-plain-row-again",
        "and the plain row is answerable again after the move");
    xud_check(xud_test_audio_close(nodeVolume, 0.5f) && xud_test_audio_close(nodePan, -0.5f), "spatial-device-plain-untouched",
        "and answers the same two values, so the walk a listener move runs skips a handle that was never placed rather than rewriting it");
    /* The placed handle: the node hears the geometry, the getters answer the base, and the two move
       apart by exactly the arithmetic the headless half pinned.
    */
    xud_check(xud_set_audio_listener_position(context, zero) == XUD_OK, "spatial-device-ear-home",
        "the ear goes back to the world origin so the numbers below are the ones the headless half states");
    xud_check(xud_set_audio_volume(context, placed, 0.8f) == XUD_OK, "spatial-device-base-asked",
        "a level that is neither the default nor the silence is asked of the handle that has never been placed");
    xud_check(xud_set_audio_source_position(context, placed, place) == XUD_OK, "spatial-device-placed",
        "and the handle is placed five along -x, which makes it spatial with a row already under it");
    xud_check(xud_test_audio_node(&nodeVolume, &nodePan, context, placed) == 1, "spatial-device-placed-row",
        "the placed handle's row is answerable");
    xud_check(xud_test_audio_close(nodeVolume, 0.16f), "spatial-device-node-is-rolloff",
        "and the node hears 0.8/5 - the base over the five the place put between the ear and the source, which is the write the level's "
        "setter ran instead of the 0.8 it was handed");
    xud_check(xud_test_audio_close(nodePan, 1.0f), "spatial-device-node-is-pan",
        "and the full right, this facing's derived right being -x and the source sitting on it");
    xud_check(xud_get_audio_volume(&volume, context, placed) == XUD_OK, "spatial-device-base-answers", "and the level's question answers");
    xud_check(volume == 0.8f, "spatial-device-base-survives",
        "and answers the 0.8 the caller asked rather than the 0.16 the node was told, the two states being one record apart");
    xud_check(xud_get_audio_pan(&pan, context, placed) == XUD_OK, "spatial-device-base-pan-answers",
        "and the position's question answers beside the level's");
    xud_check(pan == 0.0f, "spatial-device-base-pan-survives",
        "and answers the centre the caller last asked for, not the full right the row was handed");

    xud_check(xud_set_audio_listener_position(context, place) == XUD_OK, "spatial-device-ear-at-source",
        "the ear is moved onto the source, which is the one call a frame that moved the camera makes");
    xud_check(xud_test_audio_node(&nodeVolume, &nodePan, context, placed) == 1, "spatial-device-follow-row",
        "and the placed row is answerable after it");
    xud_check(xud_test_audio_close(nodeVolume, 0.8f) && xud_test_audio_close(nodePan, 0.0f), "spatial-device-follows-the-ear",
        "and the node now hears the base at the centre: zero distance rolls off over nothing and has no direction to pan by, so moving the "
        "ear rewrote a row nobody touched");

    xud_check(xud_set_audio_listener_position(context, zero) == XUD_OK, "spatial-device-ear-home-again", "the ear goes home again");
    read.x = 1.0f;
    read.y = 0.0f;
    read.z = 0.0f;
    xud_check(xud_set_audio_listener_forward(context, read) == XUD_OK, "spatial-device-turn",
        "and the ear turns down +x - the second setter that recomputes, the facing being half of the pan");
    xud_check(xud_test_audio_node(&nodeVolume, &nodePan, context, placed) == 1, "spatial-device-turned-row", "and the row answers");
    xud_check(xud_test_audio_close(nodeVolume, 0.08f), "spatial-device-turned-behind",
        "and the source at -x is now straight BEHIND that facing, so the half the headless half pinned is applied over the rolloff: 0.8 "
        "over "
        "the five, halved");
    xud_check(xud_test_audio_close(nodePan, 0.0f), "spatial-device-turned-centred",
        "and straight behind the new facing is on the new right by zero, so the centre is what the node hears");
    xud_check(xud_get_audio_source_position(&read, context, placed) == XUD_OK, "spatial-device-place-answers",
        "and the source's question answers through the real context");
    xud_check((read.x == -5.0f) && (read.y == 0.0f) && (read.z == 0.0f), "spatial-device-place-untouched-by-the-ear",
        "and still the place the setter asked for, the ear's two moves having rewritten the node and not the record");

    /* A handle placed BEFORE its first play: the row the play builds takes the spatial value rather
       than the base, which is the one recompute the two setters cannot do for a row that did not
       exist yet.
    */
    xud_check(xud_stop_audio(context, plain) == XUD_OK, "spatial-device-plain-stop", "the plain handle is stopped before its close");
    xud_check(xud_close_audio(context, plain) == XUD_OK, "spatial-device-plain-close", "and closed");
    audio.fileData = xudTestAudioFile[1];
    audio.fileBytes = xudTestAudioBytes[1];
    xud_check(xud_load_audio(&plain, context, &audio) == XUD_OK, "spatial-device-late-loaded",
        "a third stream takes the slot for the play-after-the-place");
    xud_check(xud_set_audio_listener_forward(context, longForward) == XUD_OK, "spatial-device-ear-facing-plus-z",
        "and the ear faces +z again, the same facing it came up with");
    xud_check(xud_set_audio_volume(context, plain, 0.5f) == XUD_OK, "spatial-device-late-base", "a base is asked of it");
    xud_check(xud_set_audio_source_position(context, plain, place) == XUD_OK, "spatial-device-late-place",
        "and it is placed five along -x BEFORE it has ever been played, so there is no row to write yet");
    played = xud_play_audio(context, plain);
    xud_check(played == XUD_OK, "spatial-device-late-play", "and the play builds the row it never had");
    xud_check(xud_test_audio_node(&nodeVolume, &nodePan, context, plain) == 1, "spatial-device-late-row", "the new row answers");
    xud_check(xud_test_audio_close(nodeVolume, 0.1f) && xud_test_audio_close(nodePan, 1.0f), "spatial-device-late-row-is-spatial",
        "and it is built at the SPATIAL pair - 0.5 over the five, and the full right - rather than at the base for one frame and the "
        "geometry after it, which is the play taking the record's place as well as its bases");

    xud_check(xud_stop_audio(context, placed) == XUD_OK, "spatial-device-placed-stop", "the placed handle is stopped");
    xud_check(xud_close_audio(context, placed) == XUD_OK, "spatial-device-placed-close", "and closed, position and bases and all");
    xud_check(xud_stop_audio(context, plain) == XUD_OK, "spatial-device-late-stop", "and the late one beside it");
    xud_check(xud_close_audio(context, plain) == XUD_OK, "spatial-device-late-close", "and closed");
    xud_check(xud_sound_close(&context->sound, &closed) == XUD_OK, "spatial-device-sound-close",
        "the rung's own teardown closes the device the listener lived on");
    xud_check(context->sound == NULL, "spatial-device-sound-cleared", "and is cleared through the context's own copy");
    xud_check(xud_close_context(&context) == XUD_OK, "spatial-device-context-closed", "and the context closes over it");
    xud_check(context == NULL, "spatial-device-context-null", "and is cleared, leaving no row, no ear and no block behind");
}

/*-----------------------------------------------------------------------------------------------
 * The area's entry point
 *-----------------------------------------------------------------------------------------------*/

void xud_test_audio(void) {
    if (xud_test_audio_file_read() == 0) {
        printf("note: no audio files at resources/audio, so the decode and the GPU half are skipped\n");
        xud_test_audio_check();
        xud_test_audio_codes();
        xud_test_audio_stride();
        xud_test_audio_time();
        xud_test_audio_spatial();
        return;
    }

    xud_test_audio_expect();
    xud_test_audio_check();
    xud_test_audio_codes();
    xud_test_audio_stride();
    xud_test_audio_time();
    xud_test_audio_decode();
    xud_test_audio_refused();
    xud_test_audio_shell();
    xud_test_audio_play();
    xud_test_audio_params();
    xud_test_audio_spatial_shell();
    xud_test_audio_spatial();
    if (xud_gpu_tests_enabled()) {
        xud_test_audio_gpu();
        xud_test_audio_device();
        xud_test_audio_spatial_device();
    }

    xud_test_audio_file_drop();
}
