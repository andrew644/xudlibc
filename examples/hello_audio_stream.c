/***********************************************************************************************
 *
 *   hello_audio_stream.c - one stream playing while frames are drawn: the two clocks, seen
 *
 *   Owns : one window, one font for the state overlay, one stream read off disk and played
 *          through the sound rung, the frame loop that draws a scene and one state line per
 *          frame while the stream plays, ends and plays again, one scheduled volume change on
 *          a playing node, and the report that counts the frames of each kind. Nothing else:
 *          no camera, no input, no second stream.
 *
 *   Layer: examples. Public API only (xud.h) plus stdio for the lines it prints, stdlib for
 *          the two buffers the files are read into and math for the scene's orbit and pulse -
 *          nothing else, which is why the frame loop reads the clock rung (xud_get_time)
 *          rather than sleeping, and why the "is it playing" question is the public
 *          xud_is_audio_playing rather than anything this file names.
 *
 *   The point of the example: that the audio thread and the frame loop coexist is something
 *          that can be seen rather than assumed. The stream is two seconds and the run is
 *          six, so the frame loop watches the stream end while frames keep being drawn, sits
 *          out one visible gap, and plays the stream again - every playing-to-ended
 *          transition is a state the audio thread made and the frame loop read back through
 *          the public question, and every ended-to-playing one is a state the frame loop made
 *          and the audio thread then walked. The overlay line shows which kind of state each
 *          frame is in, and the report at the end counts the two clocks' agreement: how many
 *          frames each ran while the other was doing its job.
 *
 *   The scene is drawn for the same reason the window is: a frame loop with nothing in it is
 *          a loop the reader could not see. A cube orbiting a sphere, and a line between the
 *          two, are the smallest scene that moves, and the movement is a function of the
 *          frame loop's own clock, so every frame differs from the one before it by the frame
 *          loop's time alone - which is what the audio thread's time cannot do to it. The
 *          frame has no camera, so the scene is in the clip space the identity viewProj lands
 *          a draw in: x and y are the screen's [-1,1] and z is the depth the two bodies sit at.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 **********************************************************************************************/

#include <math.h>   /* Required for: cosf and sinf, the orbit and the pulse the scene walks */
#include <stdio.h>  /* Required for: fopen, fseek, ftell, fread, fclose, snprintf and printf, the file reads and the report */
#include <stdlib.h> /* Required for: malloc and free, the two buffers the files are read into */

#include "xud.h" /* Required for: XudAudioData, XudAudioInfo, XudColor, XudContext, XudFontData, XudHandle, XudResult and XudVector3, the
                   xud_init_context/xud_close_context pair, xud_window_should_close, the xud_begin_drawing/xud_end_drawing pair, xud_clear_background,
                   xud_draw_cube/xud_draw_sphere/xud_draw_line_3d, xud_draw_text_screen, xud_get_time, the xud_load_font/xud_close_font pair, the
                   xud_load_audio/xud_close_audio pair, xud_get_audio_info/xud_get_audio_length, xud_play_audio/xud_stop_audio, xud_is_audio_playing,
                   xud_set_audio_volume, xud_result_name and XUD_VERSION - the whole public surface above the frame loop and both rungs, and no
                   internal header */

/*
 * The window, the files and the run:
 *
 * 640: the width in pixels, and 360 the height: the swapchain this context creates is the owner's cost, not this example's content, and a
 *      size that reads as a scene would be a claim about content the scene below does not make.
 *
 * resources/audio/target.ogg: the stream, read from the gitignored resources/ dir rather than generated: the run below is stated against
 *      one file the repo carries, so the two-second walk it plays and re-plays is the same walk on every machine, and a length the run knows
 *      about is what makes the end-and-restart below a schedule rather than a surprise.
 *
 * resources/fonts/NotoSansTC-Regular.ttf: the overlay's font, the same file hello_font bakes, for the same reason: one file the repo
 *      carries rather than whichever collection a machine happens to ship, so the overlay measures the same on every run.
 *
 * 24: the overlay's em height, in pixels.
 *
 * 6.0: the length this run takes, in seconds of the clock rung: three walks of a two-second stream with a half-second gap between each end,
 *      which is what leaves a frame of every kind - playing, ended, and one that asked the setter - drawn rather than assumed.
 *
 * 60000: the frame cap, the guard every other example's loop carries: the clock is what ends the run, the cap is what ends a clock that
 *      stops moving, and the number is large enough that a real run at any sane frame rate never sees it.
 *
 * 0.5: the gap, in seconds of the clock rung, that the frame loop sits out between a stream's end and its replay: long enough that frames
 *      of the gap kind are visible in the count and in a screenshot, short enough that the run above still finishes three walks.
 *
 * 3.0 and 0.6: the one scheduled volume change: at three seconds into the run, on a node that is already playing, to 0.6 - the state that a
 *      setter on a playing node is a live ask rather than a bring-up, and the report's second line is that ask and its answer.
 *
 * 64: the longest overlay line is under 45 codepoints, so 64 carries it with the room snprintf needs for its own NUL.
 */
#define XUD_HELLO_WINDOW_WIDTH 640u
#define XUD_HELLO_WINDOW_HEIGHT 360u

#define XUD_HELLO_AUDIO_PATH "resources/audio/target.ogg"
#define XUD_HELLO_FONT_PATH "resources/fonts/NotoSansTC-Regular.ttf"
#define XUD_HELLO_FONT_PIXEL_SIZE 24u

#define XUD_HELLO_RUN_SECONDS 6.0
#define XUD_HELLO_FRAME_CAP 60000u

#define XUD_HELLO_END_GAP_SECONDS 0.5
#define XUD_HELLO_VOLUME_CHANGE_SECONDS 3.0
#define XUD_HELLO_VOLUME_CHANGED 0.6f

#define XUD_HELLO_STREAM_LINE_MAX 64u

/*-----------------------------------------------------------------------------------------------
 * The file read: the one helper the example keeps, the same way hello_audio and hello_font each keep their own
 *-----------------------------------------------------------------------------------------------*/

/* One container into one buffer: the bytes are the load's only input, so the read is the whole preparation the example makes, and a file
   that cannot be opened, sized or read in full answers NULL rather than a partial buffer the decoder would then refuse. */
static uint8_t* xud_hello_stream_read(const char* path, uint32_t* outBytes) {
    long size = 0L;
    uint8_t* buffer = NULL;
    FILE* file = fopen(path, "rb");

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
 * The pen: the one codepoint walk the overlay's lines share
 *-----------------------------------------------------------------------------------------------*/

/* The ASCII bytes straight into the codepoint array: the overlay's lines are ASCII by construction, so the walk is one byte to one codepoint,
   and the count it answers is the textCount the draw below takes rather than a strlen the caller would then have to repeat. */
static uint32_t xud_hello_stream_points(const char* text, uint32_t* outPoints) {
    uint32_t count = 0u;

    while (text[count] != '\0') {
        outPoints[count] = (uint32_t)(uint8_t)text[count];
        count++;
    }
    return count;
}

/*-----------------------------------------------------------------------------------------------
 * One frame: the scene the frame loop's clock walks, and the overlay that names the state
 *-----------------------------------------------------------------------------------------------*/

/* One frame's contents: the 3D scene and the overlay's four lines. The scene is a function of t, the frame loop's own clock, and the
   overlay's first dynamic line is the state the caller's is_audio_playing question answered, so a frame carries both clocks' agreement in
   the same draw. The scene has no camera, so the bodies are in the clip space the identity viewProj lands a draw in: x and y are the
   screen's [-1,1] and z is the depth a body sits at, the cube nearer than the sphere and the line between them. */
static XudResult xud_hello_stream_draw(XudContext* context, XudHandle font, double t, const char* stateLine, XudColor stateColor,
    const char* clockLine, const char* countLine) {
    const float angle = (float)t * 1.5f;
    const XudVector3 cube = { .x = cosf(angle) * 0.40f, .y = 0.0f, .z = 0.2f };
    const XudVector3 sphere = { .x = 0.0f, .y = 0.0f, .z = 0.5f };
    const float radius = 0.10f + 0.03f * sinf((float)t * 3.0f);
    uint32_t points[XUD_HELLO_STREAM_LINE_MAX];
    uint32_t count = 0u;
    XudResult result;

    result = xud_clear_background(context, (XudColor) { .r = 16u, .g = 18u, .b = 24u, .a = 255u });
    if (result != XUD_OK) {
        return result;
    }

    /* The scene: the orbit is the frame loop's time alone, and the line between the two bodies is what makes the orbit a relationship
       rather than two moving things. */
    result = xud_draw_cube(context, cube, 0.15f, 0.15f, 0.15f, (XudColor) { .r = 255u, .g = 120u, .b = 90u, .a = 255u });
    if (result != XUD_OK) {
        return result;
    }
    result = xud_draw_sphere(context, sphere, radius, (XudColor) { .r = 90u, .g = 120u, .b = 255u, .a = 255u });
    if (result != XUD_OK) {
        return result;
    }
    result = xud_draw_line_3d(context, sphere, cube, (XudColor) { .r = 90u, .g = 90u, .b = 120u, .a = 255u });
    if (result != XUD_OK) {
        return result;
    }

    /* The overlay: the title first, the state the caller answered next, the clock the loop is walking third, and the count the report
       will finish fourth. */
    count = xud_hello_stream_points("hello_audio_stream", points);
    result = xud_draw_text_screen(context, font, points, count, 10, 10, (float)XUD_HELLO_FONT_PIXEL_SIZE, 0.0f,
        (XudColor) { .r = 255u, .g = 255u, .b = 255u, .a = 255u });
    if (result != XUD_OK) {
        return result;
    }
    count = xud_hello_stream_points(stateLine, points);
    result = xud_draw_text_screen(context, font, points, count, 10, 42, (float)XUD_HELLO_FONT_PIXEL_SIZE, 0.0f, stateColor);
    if (result != XUD_OK) {
        return result;
    }
    count = xud_hello_stream_points(clockLine, points);
    result = xud_draw_text_screen(context, font, points, count, 10, 74, (float)XUD_HELLO_FONT_PIXEL_SIZE, 0.0f,
        (XudColor) { .r = 220u, .g = 220u, .b = 230u, .a = 255u });
    if (result != XUD_OK) {
        return result;
    }
    count = xud_hello_stream_points(countLine, points);
    result = xud_draw_text_screen(context, font, points, count, 10, 106, (float)XUD_HELLO_FONT_PIXEL_SIZE, 0.0f,
        (XudColor) { .r = 170u, .g = 170u, .b = 190u, .a = 255u });
    return result;
}
int main(void) {
    uint32_t audioBytes = 0u;
    uint32_t fontBytes = 0u;
    uint8_t* audioData = xud_hello_stream_read(XUD_HELLO_AUDIO_PATH, &audioBytes);

    if (audioData == NULL) {
        printf("hello_audio_stream: no container at %s, so there is nothing to play\n", XUD_HELLO_AUDIO_PATH);
        return 1;
    }
    uint8_t* fontData = xud_hello_stream_read(XUD_HELLO_FONT_PATH, &fontBytes);

    if (fontData == NULL) {
        printf("hello_audio_stream: no font at %s, so there is no overlay to draw the state in\n", XUD_HELLO_FONT_PATH);
        free(audioData);
        return 1;
    }

    XudContext* context = NULL;
    XudResult step = xud_init_context(&context, XUD_HELLO_WINDOW_WIDTH, XUD_HELLO_WINDOW_HEIGHT, "hello_audio_stream");

    printf("hello_audio_stream: xud_init_context(%u, %u, \"%s\") -> %s (xudlibc %s)\n", XUD_HELLO_WINDOW_WIDTH, XUD_HELLO_WINDOW_HEIGHT,
        "hello_audio_stream", xud_result_name(step), XUD_VERSION);
    if (step != XUD_OK) {
        free(audioData);
        free(fontData);
        return (step == XUD_UNSUPPORTED) ? 0 : 1;
    }

    /* The overlay's font, baked at the run's own em height: the atlas the bake owns is a TEXTURE slot of the context's handle table,
       so a context closed with the font still live would be a table freed under a record that names an image. */
    const XudFontData font = (XudFontData) {
        .fileData = fontData,
        .fileBytes = fontBytes,
        .pixelSize = XUD_HELLO_FONT_PIXEL_SIZE,
        .codepoints = NULL,
        .codepointCount = 0u,
    };
    XudHandle fontHandle;

    step = xud_load_font(&fontHandle, context, &font);
    printf("hello_audio_stream: xud_load_font(%s, %u px) -> %s\n", XUD_HELLO_FONT_PATH, (unsigned)XUD_HELLO_FONT_PIXEL_SIZE,
        xud_result_name(step));
    if (step != XUD_OK) {
        printf("hello_audio_stream: the font was refused, so there is no overlay to draw the state in\n");
        (void)xud_close_context(&context);
        free(audioData);
        free(fontData);
        return 1;
    }

    /* The stream, decoded from the bytes in one call: the container is autodetected from the bytes rather than trusted from the
       extension, the same way hello_audio states its loads, so a wrapper this tree reads answers a stream and one it does not answers
       a refusal before a slot is spent. */
    const XudAudioData audio = (XudAudioData) { .fileData = audioData, .fileBytes = audioBytes };
    XudHandle stream;

    step = xud_load_audio(&stream, context, &audio);
    printf("hello_audio_stream: xud_load_audio(%s, %u bytes) -> %s\n", XUD_HELLO_AUDIO_PATH, audioBytes, xud_result_name(step));
    if (step != XUD_OK) {
        printf("hello_audio_stream: the container was refused, so there is nothing to play\n");
        (void)xud_close_font(context, fontHandle);
        (void)xud_close_context(&context);
        free(audioData);
        free(fontData);
        return 1;
    }

    /* The info first and the length second, the same order hello_audio argues for: the length is the frame count divided by the rate,
       so the numbers printed are the ones the number below was computed from. The length is also the number the run below schedules
       against - the walk the stream takes, and re-takes. */
    XudAudioInfo info;
    float lengthSeconds = 0.0f;

    step = xud_get_audio_info(&info, context, stream);
    if (step == XUD_OK) {
        step = xud_get_audio_length(&lengthSeconds, context, stream);
    }
    if (step != XUD_OK) {
        printf("hello_audio_stream: xud_get_audio_info/xud_get_audio_length -> %s, so the run cannot say what was loaded\n",
            xud_result_name(step));
        (void)xud_close_audio(context, stream);
        (void)xud_close_font(context, fontHandle);
        (void)xud_close_context(&context);
        free(audioData);
        free(fontData);
        return 1;
    }
    printf("hello_audio_stream: %s -> %u channel(s) to %u Hz, %llu frame(s) = %.3f s, the walk the frame loop then takes and re-takes\n",
        XUD_HELLO_AUDIO_PATH, info.channels, info.sampleRate, (unsigned long long)info.frameCount, (double)lengthSeconds);

    /* The first play, before the loop: the play hands the row to the device's own thread, which pulls frames at real-time rate, so the
       coexistence the example is about starts here rather than inside the loop, and a refusal with XUD_UNSUPPORTED - no backend this
       machine can drive - is an answer the run draws around rather than a crash: the frame loop still runs, still shows its state, and
       the report still counts, only the state line says what the machine cannot do. */
    bool hasBackend = false;
    uint32_t plays = 0u;

    step = xud_play_audio(context, stream);
    printf("hello_audio_stream: xud_play_audio -> %s, heard through the engine's own device\n", xud_result_name(step));
    if (step == XUD_OK) {
        hasBackend = true;
        plays = 1u;
    } else {
        printf("hello_audio_stream: no audio this machine can drive, so the run draws without it\n");
    }

    /* The frame loop: each frame reads the clock, asks the public question the sound rung keeps,
       answers the one scheduled setter, and draws the scene and the state the two clocks now agree on. */
    double t0 = 0.0;
    double now = 0.0;
    double t = 0.0;
    double endedAt = 0.0;
    bool volumeChanged = false;
    double volumeChangedAt = 0.0;
    float volume = 1.0f;
    uint32_t framesPlaying = 0u;
    uint32_t framesEnded = 0u;
    uint32_t shortFrames = 0u;
    uint32_t frame = 0u;

    step = xud_get_time(&t0, context);
    if (step != XUD_OK) {
        printf("hello_audio_stream: xud_get_time -> %s, so the run cannot say where it started\n", xud_result_name(step));
    }
    while (frame < XUD_HELLO_FRAME_CAP) {
        step = xud_get_time(&now, context);
        if (step != XUD_OK) {
            printf("hello_audio_stream: frame %u xud_get_time -> %s, and the run stops here\n", frame, xud_result_name(step));
            break;
        }
        t = now - t0;
        if (t >= XUD_HELLO_RUN_SECONDS) {
            break;
        }

        /* The state, asked BEFORE the frame it labels: the frame is drawn with the answer the question
           gives now, so the line a frame shows is the state the audio thread had when that frame's draw
           calls were appended, and a playing-to-ended transition is visible on the frame that first
           sees it rather than the one after. */
        const char* stateText = "no audio backend";
        XudColor stateColor = (XudColor) { .r = 200u, .g = 120u, .b = 120u, .a = 255u };

        if (hasBackend) {
            bool playing = false;

            step = xud_is_audio_playing(&playing, context, stream);
            if (step != XUD_OK) {
                printf("hello_audio_stream: frame %u xud_is_audio_playing -> %s, and the run stops here\n", frame, xud_result_name(step));
                break;
            }
            if (playing) {
                framesPlaying++;
                stateText = "playing";
                stateColor = (XudColor) { .r = 120u, .g = 230u, .b = 140u, .a = 255u };
            } else {
                framesEnded++;
                stateText = "ended";
                stateColor = (XudColor) { .r = 240u, .g = 190u, .b = 90u, .a = 255u };

                /* The gap: the end is observed once (the frame that first sees it), the gap is then a
                   schedule of the frame loop's own clock, and the replay is what the frame loop gives
                   the audio thread to walk again - a state made here rather than there. */
                if (endedAt == 0.0) {
                    endedAt = t;
                } else if (t - endedAt >= XUD_HELLO_END_GAP_SECONDS) {
                    endedAt = 0.0;
                    step = xud_play_audio(context, stream);
                    if (step == XUD_OK) {
                        plays++;
                        framesPlaying++;
                        framesEnded--;
                        stateText = "playing";
                        stateColor = (XudColor) { .r = 120u, .g = 230u, .b = 140u, .a = 255u };
                    } else {
                        printf("hello_audio_stream: frame %u xud_play_audio -> %s, so the run stops restarting\n", frame,
                            xud_result_name(step));
                        hasBackend = false;
                        stateText = "replay refused";
                        stateColor = (XudColor) { .r = 200u, .g = 120u, .b = 120u, .a = 255u };
                    }
                }
            }
        }

        /* The one scheduled volume change: at three seconds in, on a node that is already playing,
           which is what a setter on a live node is for - the ask goes through the public call and the
           report's second line is that ask and its answer. */
        if (hasBackend && !volumeChanged && t >= XUD_HELLO_VOLUME_CHANGE_SECONDS) {
            step = xud_set_audio_volume(context, stream, XUD_HELLO_VOLUME_CHANGED);
            if (step != XUD_OK) {
                printf("hello_audio_stream: frame %u xud_set_audio_volume -> %s\n", frame, xud_result_name(step));
            } else {
                volumeChanged = true;
                volumeChangedAt = t;
                volume = XUD_HELLO_VOLUME_CHANGED;
            }
        }

        char stateLine[XUD_HELLO_STREAM_LINE_MAX];
        char clockLine[XUD_HELLO_STREAM_LINE_MAX];
        char countLine[XUD_HELLO_STREAM_LINE_MAX];

        snprintf(stateLine, XUD_HELLO_STREAM_LINE_MAX, "target.ogg: %s", stateText);
        snprintf(clockLine, XUD_HELLO_STREAM_LINE_MAX, "t = %.2f s   frame = %u", t, frame);
        snprintf(countLine, XUD_HELLO_STREAM_LINE_MAX, "plays %u   vol %.2f   frames %u/%u playing", plays, (double)volume, framesPlaying,
            frame + 1u);

        step = xud_begin_drawing(context);
        if (step != XUD_OK) {
            printf("hello_audio_stream: frame %u xud_begin_drawing -> %s, and the run stops here\n", frame, xud_result_name(step));
            break;
        }
        step = xud_hello_stream_draw(context, fontHandle, t, stateLine, stateColor, clockLine, countLine);
        if (step == XUD_OUT_OF_MEMORY) {
            shortFrames++;
            step = XUD_OK;
        } else if (step != XUD_OK) {
            printf("hello_audio_stream: frame %u draw -> %s, and the run stops here\n", frame, xud_result_name(step));
            break;
        }
        step = xud_end_drawing(context);
        if (step != XUD_OK) {
            printf("hello_audio_stream: frame %u xud_end_drawing -> %s, and the run stops here\n", frame, xud_result_name(step));
            break;
        }
        bool shouldClose = false;

        step = xud_window_should_close(&shouldClose, context);
        if (step != XUD_OK) {
            printf("hello_audio_stream: frame %u xud_window_should_close -> %s, and the run stops here\n", frame, xud_result_name(step));
            break;
        }
        if (shouldClose) {
            break;
        }
        frame++;
    }
    /* The report: the two clocks' agreement, counted rather than claimed. The frame count is the loop's, the playing and ended counts are
       the state the is_audio_playing question answered per frame, and the plays count is the frame loop's own - the first play before the
       loop and each replay inside it, which is what makes the number a fact about the restart schedule rather than about the stream. */
    printf("hello_audio_stream: %u frames in %.2f s: %u while playing, %u while ended, %u short, %u plays\n", frame, (double)t,
        framesPlaying, framesEnded, shortFrames, plays);
    if (volumeChanged) {
        printf("hello_audio_stream: xud_set_audio_volume(%.2f) answered on a playing node at %.2f s\n", (double)XUD_HELLO_VOLUME_CHANGED,
            volumeChangedAt);
    }

    /* The stop before the close: a stream closed while its row is playing refuses - the detach above the close refuses rather than
       stopping on the caller's behalf - so the stop is the answer that refusal would ask for, the same way hello_audio states it, and
       the skip when there was no backend is the state that nothing is playing rather than a call into an engine that never came up. */
    if (hasBackend) {
        const XudResult stopped = xud_stop_audio(context, stream);

        printf("hello_audio_stream: xud_stop_audio -> %s\n", xud_result_name(stopped));
        if (stopped != XUD_OK) {
            step = stopped;
        }
    }

    /* The stream and the font given back before the context: each is a record the context's handle table names - an AUDIOSTREAM slot
       that owns a block the decoder allocated, a FONT slot that owns an image - and the table is the top rung xud_close_context() retires
       first, so a context closed with either still live would be a table freed under a record that names a block or an image. */
    const XudResult audioClosed = xud_close_audio(context, stream);

    printf("hello_audio_stream: xud_close_audio -> %s\n", xud_result_name(audioClosed));
    const XudResult fontClosed = xud_close_font(context, fontHandle);

    printf("hello_audio_stream: xud_close_font -> %s\n", xud_result_name(fontClosed));
    const XudResult closed = xud_close_context(&context);

    printf("hello_audio_stream: xud_close_context -> %s\n", xud_result_name(closed));

    free(audioData);
    free(fontData);
    return ((step != XUD_OK) || (audioClosed != XUD_OK) || (fontClosed != XUD_OK) || (closed != XUD_OK)) ? 1 : 0;
}
