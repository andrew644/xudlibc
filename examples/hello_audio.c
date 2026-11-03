/***********************************************************************************************
 *
 *   hello_audio.c - one container's bytes, decoded into the stream behind one handle
 *
 *   Owns : two containers read off disk, one context, two streams loaded from the bytes the
 *          decoder reads, the three numbers and the one length each stream answers, one stream
 *          played and stopped through the sound rung, and the teardown that gives the streams
 *          back before it gives back the context. Nothing else: no draw call, no camera, no bat
 *          ch. The play call, the mixer and the device ARE here now, and they are here through t
 *          he public pair rather than through anything this file names.
 *
 *   Layer: examples. Public API only (xud.h) plus stdio for the lines it prints and stdlib for
 *          the one buffer a container is read into - and nothing else, which is why the wait below
 *          reads the clock rung rather than sleeping through a libc call. That is the whole point
 *          of the public header here: user code names a FILE and a handle, and never a miniaudio
 *          call, a decoder config, a heap block, a graph node or a device. The one thing the
 *          public header cannot do for a caller is read the file: xud_load_audio takes BYTES,
 *          not a path (see the note beside XudAudioData in xud.h, which states the other half
 *          of that split - a path would make this call a file-system read, and the loader rung
 *          that reads a model's files is a different rung's decision), so the read below is the
 *          example's own and is the shortest honest one.
 *
 *   Why a window exists at all when nothing is drawn: every audio slot belongs to the context,
 *   which is the single owner of everything the library creates, and xud_load_audio's first
 *   argument is that context. The audio rung is therefore ABOVE the bring-up rather than beside
 *   it - a stream is a slot of the context's own handle table, and a decode that ran with no
 *   context would have nowhere to spend one. What this example spends is one window and one
 *   swapchain it never draws into, and that is the honest cost of the API as it stands today
 *   rather than a stub: the window is not decoration, it is the owner.
 *
 *   What this example plays, and what it still prints: the second audio rung landed, so the ogg
 *   wrapper is HEARD as well as printed - xud_play_audio/xud_stop_audio, raylib's PlayAudio/StopA
 *   udio half, above miniaudio's engine (its ma_device, its node graph and its mixer). The numbers
 *   are still printed beside it for the reason the print is a fact and the play is an event: the
 *   three numbers and the one length are what a run says, and the two seconds of the same recordi
 *   ng are what a run does, and a run that only heard would be a run a reader could not check.
 *   Only the first wrapper is played, and that is the point rather than an oversight: the pair is
 *   one recording in two wrappers, so hearing it twice would be hearing the same fact twice, and
 *   the second stream's row is never spent, which is the lazy spend the sound rung's own note desc
 *   ribes - an audio device is spent on the first play, not at bring-up, and a stream that was nev
 *   er played is a stream whose close has no row to refuse. The play is asynchronous, so the examp
 *   le waits out the length it printed before it stops and closes: the device's own thread pulls f
 *   rames at real-time rate, and a run that walked on would exit mid-walk and hear a tenth of a sec
 *   ond of the recording rather than the recording.
 *
 *   The two containers, and why they are two: they are the SAME track in two wrappers, which is
 *   what makes the pair one statement about the decoders rather than two about the files. The
 *   ogg wrapper is walked by stb_vorbis and the flac one by dr_flac, so a run prints the same
 *   channel count, the same rate and the same frame count twice over, and the agreement is a
 *   fact about the recording rather than about whichever decoder walked it. The ogg decoder
 *   came in as an ask to include stb_vorbis rather than as a decoder written here (AGENTS.md's
 *   rule), and it is inlined through src/xud/xud_vendor_miniaudio.c, the one TU that decides
 *   what miniaudio compiles - which is why the wrappers this tree reads are a property of the
 *   vendored copy rather than a choice this rung made.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include <stdio.h> /* Required for: fopen, fseek, ftell, fread, fclose and printf, the file read and the report */
#include <stdlib.h> /* Required for: malloc and free, the one buffer a container is read into */

#include "xud.h" /* Required for: XudAudioData, XudAudioInfo, XudContext, xud_load_audio/xud_close_audio, xud_get_audio_info, xud_get_audio_length and xud_play_audio/xud_stop_audio - the whole public audio API above both rungs, and no internal header */

/*-----------------------------------------------------------------------------------------------
 * The window, the containers and the run
 *-----------------------------------------------------------------------------------------------*/

/* The window is never drawn into, and the size is therefore the smallest honest one rather than
   the 16:9 the other examples argue about: the swapchain this context creates is the owner's
   cost of the audio API, not this example's content, and an extent that reads as a scene would
   be a claim about content there is none of. */
#define XUD_HELLO_WINDOW_WIDTH 320u
#define XUD_HELLO_WINDOW_HEIGHT 180u

/* The repo's own test assets under the gitignored resources/ dir rather than a generated file or
   a system sound dir: the numbers a run prints are what the LOAD folded out of these bytes, so a
   run on any machine prints the SAME rates, counts and lengths rather than whichever sound
   collection that machine happens to ship. The two paths are one track in two wrappers, which is
   the reason the pair reads as one statement about the decoders - see the header note above. */
#define XUD_HELLO_AUDIO_OGG "resources/audio/target.ogg"
#define XUD_HELLO_AUDIO_FLAC "resources/audio/target.flac"

/* The containers this example walks, in the order it walks them: the asked-for wrapper first, so
   the line that reads as the answer to "play target.ogg" is the first line out, and the same
   take behind the other wrapper second, so the two blocks below are one fact read by two
   decoders rather than two facts about two files. */
static const char* const xudHelloAudioPaths[] = { XUD_HELLO_AUDIO_OGG, XUD_HELLO_AUDIO_FLAC };
#define XUD_HELLO_AUDIO_COUNT ((uint32_t)(sizeof(xudHelloAudioPaths) / sizeof(xudHelloAudioPaths[0])))

/*-----------------------------------------------------------------------------------------------
 * The containers, read once each
 *-----------------------------------------------------------------------------------------------*/

/* The example's own read, and the shortest honest one: xud_load_audio takes bytes rather than a
   path, so a caller decides what read means and this file is the one that decides it here. The
   buffer is the caller's for the whole load - the rung copies nothing out of it and keeps only
   what the decoder allocated for itself - so it is freed after the load and after the close,
   never before either. */
static uint8_t* xud_hello_audio_read(const char* path, uint32_t* outBytes) {
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
 * The report: the three numbers and the one length behind one handle
 *-----------------------------------------------------------------------------------------------*/

/* The info first and the length second, and the reason the order is not a choice: the length is
   the frame count divided by the rate, so the three numbers above are the ones the one number
   below was computed from, and printing them in that order is what makes a run readable as one
   derivation rather than four hopes. The frame count is printed as a walk rather than as a size,
   because a frame is one sample per channel and the block behind the handle is frameCount times
   channels of interleaved f32 - the one number a play row would step over, which is why it is
   the number this example prints last. */
static float xud_hello_audio_report(XudContext* context, XudHandle stream, const char* path) {
    XudAudioInfo info;
    float seconds = 0.0f;
    XudResult result = xud_get_audio_info(&info, context, stream);

    if (result != XUD_OK) {
        printf("hello_audio: xud_get_audio_info -> %s, so the run cannot say what was loaded\n", xud_result_name(result));
        return 0.0f;
    }
    printf("hello_audio: %s -> %u channel(s) at %u Hz\n", path, info.channels, info.sampleRate);

    result = xud_get_audio_length(&seconds, context, stream);
    if (result != XUD_OK) {
        printf("hello_audio: xud_get_audio_length -> %s, so the run cannot say how long it is\n", xud_result_name(result));
        return 0.0f;
    }
    printf("hello_audio: %llu frame(s) of interleaved f32 = %.3f s, the walk the play below then takes\n",
        (unsigned long long)info.frameCount, seconds);

    return seconds;
}

int main(void) {
    XudContext* context = NULL;
    XudResult step = xud_init_context(&context, XUD_HELLO_WINDOW_WIDTH, XUD_HELLO_WINDOW_HEIGHT);
    uint32_t row = 0u;
    uint32_t loaded = 0u;

    printf("hello_audio: xud_init_context(%u, %u) -> %s (xudlibc %s)\n", XUD_HELLO_WINDOW_WIDTH, XUD_HELLO_WINDOW_HEIGHT,
        xud_result_name(step), XUD_VERSION);
    if (step != XUD_OK) {
        return (step == XUD_UNSUPPORTED) ? 0 : 1;
    }

    for (row = 0u; row < XUD_HELLO_AUDIO_COUNT; row++) {
        uint32_t fileBytes = 0u;
        uint8_t* buffer = xud_hello_audio_read(xudHelloAudioPaths[row], &fileBytes);

        if (buffer == NULL) {
            printf("hello_audio: no container at %s, so there is nothing to decode\n", xudHelloAudioPaths[row]);
            continue;
        }

        /* The load, in one call and one description: the container is autodetected from the bytes
           rather than trusted from the extension - the name above is a label for the line below,
           not an input - so a wrapper this tree reads answers a stream and a wrapper it does not
           answers XUD_UNSUPPORTED before a slot is spent, which is what leaves the handle naming
           nothing rather than half a stream. */
        const XudAudioData audio = (XudAudioData) { .fileData = buffer, .fileBytes = fileBytes };
        XudHandle stream;

        step = xud_load_audio(&stream, context, &audio);
        printf("hello_audio: xud_load_audio(%s, %u bytes) -> %s\n", xudHelloAudioPaths[row], fileBytes, xud_result_name(step));
        if (step != XUD_OK) {
            free(buffer);
            continue;
        }
        loaded++;

        const float lengthSeconds = xud_hello_audio_report(context, stream, xudHelloAudioPaths[row]);

        /* The play, the wait and the stop, on the first wrapper only: the pair is one recording in
           two wrappers, so hearing it twice would be hearing the same fact twice, and the second
           stream is left with no row behind it, which is the sound rung spent on the first play rat
           her than at bring-up. The WAIT is the point of this block: the play hands the row to the
           device's own thread, which pulls frames at real-time rate, so a process that walked on wo
           uld exit mid-walk and the caller would hear the first tenth of a second of the recordi
           ng rather than the recording. The wait is the clock rung's own read rather than a libc s
           leep, which is what keeps this file to the public header plus stdio and stdlib, and it is
           bounded by the length the report above printed rather than by a guess. The stop is not de
           coration either: a stream closed while its row is playing refuses (the detach above the c
           lose refuses rather than stopping on the caller's behalf), so the stop is the answer that
           refusal would ask for and the close below is what releases the block. */
        if (row == 0u) {
            double waited = 0.0;

            step = xud_play_audio(context, stream);
            printf("hello_audio: xud_play_audio -> %s, heard through the engine's own device\n", xud_result_name(step));
            while (waited < (double)lengthSeconds) {
                if (xud_get_time(&waited, context) != XUD_OK) {
                    break;
                }
            }
            printf("hello_audio: waited %.3f s on the clock rung, so the walk was heard rather than cut short\n", waited);
            step = xud_stop_audio(context, stream);
            printf("hello_audio: xud_stop_audio -> %s\n", xud_result_name(step));
        }

        /* The stream given back before the context: an AUDIOSTREAM slot is a record that owns a
           block the decoder allocated, and xud_close_context() retires the table as its top rung
           - a context closed with a stream still live would be a table freed under a record that
           names that block. */
        step = xud_close_audio(context, stream);
        printf("hello_audio: xud_close_audio -> %s\n", xud_result_name(step));
        free(buffer);
    }

    printf("hello_audio: %u of %u containers decoded, %u refused, one of them heard (this vendored miniaudio reads wav, flac, mp3 and ogg; "
           "qoa and xm still "
           "refuse)\n",
        loaded, XUD_HELLO_AUDIO_COUNT, XUD_HELLO_AUDIO_COUNT - loaded);

    const XudResult closed = xud_close_context(&context);

    printf("hello_audio: xud_close_context -> %s\n", xud_result_name(closed));

    return (closed != XUD_OK) ? 1 : 0;
}
