/***********************************************************************************************
 *
 *   xud_test_audio.c - the audio rung and the public audio API above it
 *
 *   Owns : the description's argument check and its two bounds, miniaudio's codes turned into
 *          this library's, the frame-to-bytes stride arithmetic and its ceiling, the two conver
 *          sions between frames and seconds, the decode run over six real audio files of four
 *          containers through a bare handle table, the frame count and the rate and the channel
 *          count read back out of the record beside the ones the container's own header declares
 *          them, the public load/close pair and the two queries against a shell context and ag
 *          ainst a real one, and the teardown rung above the table's own close - and nothing els
 *          e: the slot arrays and the generation bumps are the handle area's, the context's own
 *          bring-up and teardown are the core area's, and the vendor area's miniaudio check is o
 *          nly that the library links, which is not that this rung reads a container.
 *
 *   Layer: tests, above the handle area and below the core area. The whole area but the last hal
 *          f runs with no driver present at all, and that is the point of it rather than a conven
 *          ience: the audio rung's driver half calls no Vulkan entry point and no driver function,
 *          so a decode is miniaudio reading a caller's bytes into a heap block, and a bare handle t
 *          able is one host allocation - which makes the decode, the record, the refusals and the te
 *          ardown rung all checkable headless where the model's and the font areas needed a device to
 *          see anything at all. The GPU half runs the same public pair against a real context, which i
 *          s where the numbers the pure half read out of the record are seen again through the public
 *          queries, one decode rather than two hopes.
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
 *   Deterministic: six fixed files, one fixed fragment length, one fixed claimed length, no time, no
 *   randomness, and the three containers this vendored miniaudio cannot read are asked about rather than
 *   skipped, so a decoder that gained ogg support would fail this area rather than quietly pass it.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include <stdio.h> /* Required for: fopen, fread, fclose, ftell, fseek and printf, the six audio files both halves read */
#include <stdlib.h> /* Required for: malloc and free, the file buffers and the bare table */
#include <string.h> /* Required for: memset, memcpy, the shells and the handles before they are filled */

#include "xud.h" /* Required for: XudResult, XudHandle, XudContext, XudAudioData, XudAudioInfo, XUD_HANDLE_KIND_AUDIOSTREAM - the public surface above */
#include "xud_audio.h" /* Required for: XudAudioStreamRecord and the rung's own check/codes/stride/conversions/access/open/close/close_all halves */
#include "xud_core.h" /* Required for: struct XudContext, whose handleTable the public calls reach */
#include "xud_handle.h" /* Required for: XudHandleTable, xud_handle_table_open/close, xud_handle_table_live_count - the slot this record lives in */
#include "xud_test_harness.h" /* Required for: xud_check, xud_gpu_tests_enabled, the area entry points */

/*-----------------------------------------------------------------------------------------------
 * The six audio files, of four containers, and what each one is the check below stated agains
 *
 *   Four are canonical RIFF PCM, which is what makes their header a declaration this rung's dec
 *   oder can be checked AGAIN rather than a result someone copied out of a decoder: the fmt chunk
 *   states the channel count and the rate and the data chunk states the byte count, so the frame
 *   count of one of these is arithmetic on the file's own numbers. One is flac and one is mp3, a
 *   nd neither declares a PCM size, so the two are stated weaker for that reason rather than for
 *   convenience. The three that are asked about and refused - ogg, qoa and xm - are refused bec
 *   ause the vendored miniaudio inlines dr_wav, dr_flac and dr_mp3 and nothing else, which is why
 *   this library has no audio-format token: the containers it cannot read are a property of the
 *   vendored copy rather than a choice this rung made.
 *-----------------------------------------------------------------------------------------------*/

#define XUD_TEST_AUDIO_WAV_A "resources/audio/coin.wav" /* canonical RIFF PCM, 8-bit mono 22050 Hz */
#define XUD_TEST_AUDIO_WAV_B "resources/audio/sound.wav" /* canonical RIFF PCM, 32-bit float mono 44100 Hz */
#define XUD_TEST_AUDIO_WAV_C "resources/audio/spring.wav" /* canonical RIFF PCM, 16-bit mono 22050 Hz */
#define XUD_TEST_AUDIO_WAV_D "resources/audio/weird.wav" /* canonical RIFF PCM, 8-bit mono 22050 Hz, a different body */
#define XUD_TEST_AUDIO_FLAC "resources/audio/target.flac" /* flac, two channels */
#define XUD_TEST_AUDIO_MP3 "resources/audio/country.mp3" /* mp3, two channels, the largest of the six */
#define XUD_TEST_AUDIO_OGG "resources/audio/target.ogg" /* a container the vendored miniaudio has no decoder for */
#define XUD_TEST_AUDIO_QOA "resources/audio/target.qoa" /* ditto */
#define XUD_TEST_AUDIO_XM "resources/audio/mini1111.xm" /* ditto */

#define XUD_TEST_AUDIO_COUNT 9u /* the six that decode and the three that refuse */
#define XUD_TEST_AUDIO_WAV_COUNT 4u /* the ones whose header declares a PCM size, and so the ones with a strong check */
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

    for (i = XUD_TEST_AUDIO_WAV_COUNT; i < 6u; i++) {
        XudResult result = XUD_INTERNAL_ERROR;

        audio.fileData = xudTestAudioFile[i];
        audio.fileBytes = xudTestAudioBytes[i];
        result = xud_audio_open(&handle, table, &audio);
        xud_check(result == XUD_OK, "decode-compressed-ok",
            "a real flac and a real mp3 decode through the same path as the wavs, which is the reason there is no per-format loader name "
            "here");
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
        }
        xud_check(xud_audio_close(table, handle) == XUD_OK, "decode-compressed-close",
            "and it closes, so the block a compressed file spent is not left behind in a slot");
        memset(&handle, 0, sizeof(handle));
        record = NULL;
    }

    for (i = 6u; i < XUD_TEST_AUDIO_COUNT; i++) {
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

    xud_check(xud_init_context(&context, 64u, 64u) == XUD_OK, "gpu-context",
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
 * The area's entry point
 *-----------------------------------------------------------------------------------------------*/

void xud_test_audio(void) {
    if (xud_test_audio_file_read() == 0) {
        printf("note: no audio files at resources/audio, so the decode and the GPU half are skipped\n");
        xud_test_audio_check();
        xud_test_audio_codes();
        xud_test_audio_stride();
        xud_test_audio_time();
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
    if (xud_gpu_tests_enabled()) {
        xud_test_audio_gpu();
    }

    xud_test_audio_file_drop();
}
