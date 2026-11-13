/***********************************************************************************************
 *
 *   audio_sound_positioning.c - raylib's audio_sound_positioning example on xudlibc
 *
 *   Owns : one context, one font for the HUD's rows, three containers read off disk and loaded as
 *          three streams, each one PLACED at a fixed world position, and one first-person camera
 *          that is also the ear: every frame the camera's eye and its facing are handed to
 *          xud_set_audio_listener_position() and xud_set_audio_listener_forward(), and the three
 *          placed streams follow without being touched again. Layer: examples. Public API only.
 *
 *   Missing : raylib's per-sound listener (AttachAudioListener, and the SetSoundPosition(camera,
 *          sound, position, maxDist) module function of its example, which takes the camera with
 *          the position) has no counterpart here, because a handle names a stream and not a sound
 *          with a listener glued to it. The listener is therefore ONE per audio device and the
 *          position ONE per handle - which is the same state, held once rather than three times.
 *          SetSoundVelocity and SetListenerVelocity are absent with the Doppler they would drive:
 *          raylib stores both and reads neither, and this library has no Doppler path at all. The
 *          example's own maxDist and its logarithmic 1/(1 + d/maxDist) attenuation are the other
 *          divergence: xud.h states one rolloff for every spatial handle - the base over the
 *          distance, plateaued inside a unit - and an example that picked its own would be an
 *          example that disagrees with the header it is documentation for.
 *
 *   Pairing : the camera pair does NOT nest inside the drawing pair the way raylib's BeginMode3D/
 *          EndMode3D do (the note camera_first_person.c states, and models_geometric_shapes.c
 *          repeats): this library writes ONE matrix per frame at the submission, so the camera is
 *          begun every frame and taken back when the run ends. The audio pair is the reverse: the
 *          three streams are placed ONCE, before the loop, and the listener is what moves - the
 *          one shape raylib's example has no equivalent of, because it rewrites all four of a
 *          sound's numbers from its camera every frame.
 *
 *   Controls: W A S D walk, the arrow keys and the mouse look (XUD_CAMERA_MODE_FIRST_PERSON), ESC
 *          quits and takes the cursor back with it. The cursor is hidden before the loop, the way
 *          camera_first_person.c hides it, because a first-person look needs deltas. The one word
 *          this run reads is "walk" as its first argument: the ear then walks a fixed path of its
 *          own - XUD_POSITIONING_WALK_SPEED units a second forward and XUD_POSITIONING_WALK_TURN
 *          degrees a second of yaw, through xud_update_camera_pro(), which is the manual half of the
 *          same rung the keyboard drives - and that is the only way this example's distances and pans
 *          can be seen to change in a run nobody is typing at.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include <math.h>   /* Required for: sqrtf, the HUD's own distance and the unit facing below */
#include <stdio.h>  /* Required for: fopen, fseek, ftell, fread, fclose, snprintf, printf */
#include <stdlib.h> /* Required for: free and malloc, the buffers the files are read into */
#include <string.h> /* Required for: strlen, the length of a HUD row */

#include "xud.h" /* Required for: the audio pair and its positional half, the camera record and xud_update_camera, the frame pair, the cube and the grid, the font pair and the screen-fixed text, the cursor pair and the context's own pair - the whole public API this example is written against */

#define LOGNAME "audio_sound_positioning"

/* raylib's own extent (its file, lines 31 and 32), so the HUD's pixel is the same pixel. */
#define XUD_POSITIONING_WIDTH 800u
#define XUD_POSITIONING_HEIGHT 450u

/* The font: the repo's own asset, read as bytes because xud_load_font() takes bytes and not a
   path, baked at an em height the HUD's rows are drawn at.
*/
#define XUD_POSITIONING_FONT_PATH "resources/fonts/NotoSansTC-Regular.ttf"
#define XUD_POSITIONING_FONT_PIXELS 16u
#define XUD_POSITIONING_LINE_MAX 128

/* The three containers, from resources/audio - the same three the audio test area decodes. They
   are read as BYTES because xud_load_audio() takes bytes and not a path, which is the split the
   hello_audio.c banner states; the three are fixed files rather than a random pick so a run of
   this example says the same thing twice.
*/
#define XUD_POSITIONING_SOUND_COUNT 3u
static const char* const xudPositioningPaths[XUD_POSITIONING_SOUND_COUNT]
    = { "resources/audio/coin.wav", "resources/audio/sound.wav", "resources/audio/weird.wav" };
static const char* const xudPositioningNames[XUD_POSITIONING_SOUND_COUNT] = { "coin.wav", "sound.wav", "weird.wav" };

/* The three world positions, fixed rather than animated: raylib's example walks its source around a
   circle on the clock, and a fixed triangle is the same fact about the rolloff stated in a way a
   screenshot can be read twice. Each source sits on the ground plane at the grid's own height.
*/
static const XudVector3 xudPositioningSources[XUD_POSITIONING_SOUND_COUNT]
    = { { .x = 6.0f, .y = 0.5f, .z = 0.0f }, { .x = -6.0f, .y = 0.5f, .z = 4.0f }, { .x = 0.0f, .y = 0.5f, .z = -8.0f } };

/* The auto-walk, in world units per second and degrees of yaw per second: the fixed path a run with
   the word "walk" on its command line walks the ear along, at the speed the interactive walk's own
   clock would have produced. It exists because a listener that never moves is a listener whose
   numbers no reader can see change, and a screenshot has no keyboard: see the Controls half of the
   banner.
*/
#define XUD_POSITIONING_WALK_SPEED 4.0f
#define XUD_POSITIONING_WALK_TURN 30.0f

/* The eye's height and the eye it opens on: a walker's height, behind the origin looking at it, so
   the nearest source is to the right of the facing on the first frame.
*/
static const XudCamera3D xudPositioningStartCamera = { .position = { .x = 0.0f, .y = 1.6f, .z = 10.0f },
    .target = { .x = 0.0f, .y = 0.5f, .z = 0.0f },
    .up = { .x = 0.0f, .y = 1.0f, .z = 0.0f },
    .fovy = 60.0f,
    .near = 0.1f,
    .far = 200.0f };

/* The palette, raylib's own colours as this library's four bytes. */
static const XudColor xudPositioningRaywhite = { .r = 245u, .g = 245u, .b = 250u, .a = 255u };
static const XudColor xudPositioningGridCenter = { .r = 128u, .g = 128u, .b = 128u, .a = 255u };
static const XudColor xudPositioningGridLine = { .r = 191u, .g = 191u, .b = 191u, .a = 255u };
static const XudColor xudPositioningRed = { .r = 230u, .g = 41u, .b = 55u, .a = 255u };
static const XudColor xudPositioningGold = { .r = 255u, .g = 203u, .b = 0u, .a = 255u };
static const XudColor xudPositioningSky = { .r = 102u, .g = 191u, .b = 255u, .a = 255u };
static const XudColor xudPositioningBlack = { .r = 0u, .g = 0u, .b = 0u, .a = 255u };
static const XudColor xudPositioningLime = { .r = 0u, .g = 158u, .b = 47u, .a = 255u };

/* The run's own auto-walk flag, set from the command line by main() before xud_run() is called:
   zero is the interactive run every reader gets, one is the fixed path.
*/
static uint32_t xudPositioningAutoWalk = 0u;

/* One source of the run: the stream that carries it, where it stands, and what it is painted. */
typedef struct XudPositioningSource {
    XudHandle handle;
    XudVector3 position;
    XudColor color;
} XudPositioningSource;

static uint8_t* xud_positioning_read(const char* path, uint32_t* outBytes);
static void xud_text_error(const char* stage, XudResult result);
static XudResult xud_positioning_text(XudContext* context, XudHandle font, int32_t posX, int32_t posY, const char* line, XudColor color);
static XudResult xud_positioning_scene(XudContext* context, const XudPositioningSource* sources);
static float xud_positioning_distance(XudVector3 from, XudVector3 to);
static float xud_positioning_pan(const XudCamera3D* camera, XudVector3 to);
static int xud_run(XudContext* context);

/*-----------------------------------------------------------------------------------------------
 * The machinery the run needs: two file readers' shared read, the run's report, and a text call
 *-----------------------------------------------------------------------------------------------*/

/* The file's bytes: xud_load_font() and xud_load_audio() both take bytes rather than a path, so
   the public header cannot read a file for the caller and the caller reads it here. NULL when
   there is no file.
*/
static uint8_t* xud_positioning_read(const char* path, uint32_t* outBytes) {
    FILE* file = NULL;
    long size = 0L;
    uint8_t* buffer = NULL;
    size_t read = 0u;

    *outBytes = 0u;
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
    buffer = malloc((size_t)size);
    if (buffer == NULL) {
        (void)fclose(file);
        return NULL;
    }
    read = fread(buffer, 1u, (size_t)size, file);
    (void)fclose(file);
    if (read != (size_t)size) {
        free(buffer);
        return NULL;
    }
    *outBytes = (uint32_t)size;
    return buffer;
}

/* One refused call said out loud: the stage the run was in, and the rung's own answer. */
static void xud_text_error(const char* stage, XudResult result) {
    printf(LOGNAME ": %s -> refused (%s)\n", stage, xud_result_name(result));
}

/* raylib's DrawText at a screen pixel: the bytes become codepoints - the public text pair takes a
   uint32_t array because a font is keyed by codepoint and not by byte - and the row is drawn
   screen-fixed, so the ear moves the scene around it rather than the string.
*/
static XudResult xud_positioning_text(XudContext* context, XudHandle font, int32_t posX, int32_t posY, const char* line, XudColor color) {
    uint32_t codepoints[XUD_POSITIONING_LINE_MAX];
    const size_t bytes = strlen(line);
    uint32_t step = 0u;

    if ((bytes == 0u) || (bytes >= sizeof(codepoints))) {
        return XUD_INVALID_ARG;
    }
    for (step = 0u; step < (uint32_t)bytes; step++) {
        codepoints[step] = (uint32_t)(uint8_t)line[step];
    }

    return xud_draw_text_screen(context, font, codepoints, (uint32_t)bytes, posX, posY, (float)XUD_POSITIONING_FONT_PIXELS, 0.0f, color);
}

/*-----------------------------------------------------------------------------------------------
 * The HUD's own reading of the geometry
 *-----------------------------------------------------------------------------------------------*/

/* The distance the HUD prints, measured the same way the rung measures it: the length of the vector
   from the ear to the source. The library answers the BASE the caller asked for rather than the
   number the geometry produced - that is xud.h's stated choice, so the getters keep answering a
   read-back of the caller's own ask - which means the audible value has no getter to print, and the
   example reads the two vectors the formula is stated from instead.
*/
static float xud_positioning_distance(XudVector3 from, XudVector3 to) {
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float dz = to.z - from.z;

    return sqrtf((dx * dx) + (dy * dy) + (dz * dz));
}

/* The pan the HUD prints, the same three lines xud.h states: the unit direction, the derived right
   and their dot. Clamped for the same reason the rung clamps it, so what the row says is a number
   the rung could have said.
*/
static float xud_positioning_pan(const XudCamera3D* camera, XudVector3 to) {
    const float dx = to.x - camera->position.x;
    const float dy = to.y - camera->position.y;
    const float dz = to.z - camera->position.z;
    const float distance = sqrtf((dx * dx) + (dy * dy) + (dz * dz));
    float fx = camera->target.x - camera->position.x;
    float fy = camera->target.y - camera->position.y;
    float fz = camera->target.z - camera->position.z;
    const float facing = sqrtf((fx * fx) + (fy * fy) + (fz * fz));
    float rx;
    float rz;
    float right;
    float pan;

    if ((distance <= 0.0f) || (facing <= 0.0f)) {
        return 0.0f;
    }
    fx /= facing;
    fy /= facing;
    fz /= facing;
    right = sqrtf((fx * fx) + (fz * fz));
    if (right <= 0.0f) {
        return 0.0f;
    }
    rx = -fz / right;
    rz = fx / right;

    pan = ((dx / distance) * rx) + ((dz / distance) * rz);
    if (pan < -1.0f) {
        pan = -1.0f;
    }
    if (pan > 1.0f) {
        pan = 1.0f;
    }

    return pan;
}

/*-----------------------------------------------------------------------------------------------
 * The scene
 *-----------------------------------------------------------------------------------------------*/

/* The ground the ear walks on and the three sources standing on it: each one a sphere and the wire
   sphere around the same centre, which is the pair every other 3D example here draws for a thing
   the eye is meant to walk around. The spheres are where the SOUNDS are - the position a sphere
   paints is the position the stream at its index was placed at, from the one line in xud_run below.
*/
static XudResult xud_positioning_scene(XudContext* context, const XudPositioningSource* sources) {
    static const XudVector3 gridCenter = { .x = 0.0f, .y = 0.0f, .z = 0.0f };
    XudResult step;
    uint32_t index = 0u;

    step = xud_draw_grid(context, gridCenter, 24u, 2.0f, xudPositioningGridCenter, xudPositioningGridLine);
    if (step != XUD_OK) {
        return step;
    }

    for (index = 0u; index < XUD_POSITIONING_SOUND_COUNT; index++) {
        step = xud_draw_sphere(context, sources[index].position, 0.5f, sources[index].color);
        if (step != XUD_OK) {
            return step;
        }
        step = xud_draw_sphere_wires(context, sources[index].position, 0.5f, xudPositioningBlack);
        if (step != XUD_OK) {
            return step;
        }
    }

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The run
 *-----------------------------------------------------------------------------------------------*/

/* One short frame said out loud, and only the first: the batch's capacity DOUBLES at the start of a
   frame, so the first frames of a scene are drawn short until the capacity reaches what it asks
   for. Every frame redraws the whole scene, so a short one costs nothing but the shapes that did
   not fit, and a refused append is not the run's end.
*/
static void xud_positioning_count_short_frame(uint32_t frame, XudResult step, uint32_t* shortFrames) {
    if (*shortFrames == 0u) {
        printf(LOGNAME ": frame %u draw %s; the batch's capacity doubles at the start of the next frame, so this\n" LOGNAME
                       ":   scene is short for the first few frames and whole from the one whose capacity fits it\n",
            frame, xud_result_name(step));
    }
    (*shortFrames)++;
}

/* Everything the run spent, given back in the order the owner wants it: the streams first, because a
   row of the sound rung names a block an audio record owns and the teardown rung runs ABOVE the
   table's own close, the font beside them, and the bytes the loader was handed only after both - a
   buffer freed before the load that read it would be the leak's twin. A handle still XUD_HANDLE_NONE
   was never spent, so there is nothing for its close to give back.
*/
static void xud_positioning_close_all(XudContext* context, const XudPositioningSource* sources, XudHandle font, uint8_t* fontFile) {
    uint32_t index = 0u;

    for (index = 0u; index < XUD_POSITIONING_SOUND_COUNT; index++) {
        if (sources[index].handle.kind != XUD_HANDLE_KIND_NONE) {
            (void)xud_close_audio(context, sources[index].handle);
        }
    }
    if (font.kind != XUD_HANDLE_KIND_NONE) {
        (void)xud_close_font(context, font);
    }
    free(fontFile);
}

static int xud_run(XudContext* context) {
    const XudColor colors[XUD_POSITIONING_SOUND_COUNT] = { xudPositioningRed, xudPositioningGold, xudPositioningSky };
    XudPositioningSource sources[XUD_POSITIONING_SOUND_COUNT];
    XudFontData fontData
        = { .fileData = NULL, .fileBytes = 0u, .pixelSize = XUD_POSITIONING_FONT_PIXELS, .codepoints = NULL, .codepointCount = 0u };
    XudCamera3D camera = xudPositioningStartCamera;
    uint8_t* fontFile = NULL;
    uint32_t fontBytes = 0u;
    uint32_t containerBytes = 0u;
    uint32_t frame = 0u;
    uint32_t shortFrames = 0u;
    uint32_t facingRefused = 0u;
    uint32_t cursorHidden = 1u;
    XudHandle font = XUD_HANDLE_NONE;
    XudResult step;
    uint32_t index = 0u;

    fontFile = xud_positioning_read(XUD_POSITIONING_FONT_PATH, &fontBytes);
    if (fontFile == NULL) {
        printf(LOGNAME ": no font at %s, so the HUD has nothing to draw with\n", XUD_POSITIONING_FONT_PATH);
        return 1;
    }
    fontData.fileData = fontFile;
    fontData.fileBytes = fontBytes;
    step = xud_load_font(&font, context, &fontData);
    if (step != XUD_OK) {
        xud_text_error("xud_load_font", step);
        free(fontFile);
        return 1;
    }
    if (xud_set_target_fps(context, 60u) != XUD_OK) {
        printf(LOGNAME ": xud_set_target_fps -> refused, so the run free-runs\n");
    }

    /* The three sources, loaded, PLACED and started, in that order. The load spends the slot, the
       place is what makes each one spatial - before it, the two ear calls below would be arithmetic
       with nothing to drive - and the play spends the audio device on the first of the three, which
       is why the listener is not set until after this loop: a device that was never spent has no
       ear to move. Each is then restarted when its own recording runs out (raylib's example does the
       same, its file's line 108), so the ear has three things to walk around for the whole run.
    */
    /* The three rows filled before anything can fail: the cleanup walk reads all three whatever the
       load did, and a row this run never reached is a row it must still be able to name as empty.
    */
    for (index = 0u; index < XUD_POSITIONING_SOUND_COUNT; index++) {
        sources[index].handle = XUD_HANDLE_NONE;
        sources[index].position = xudPositioningSources[index];
        sources[index].color = colors[index];
    }

    for (index = 0u; index < XUD_POSITIONING_SOUND_COUNT; index++) {
        const uint8_t* container = xud_positioning_read(xudPositioningPaths[index], &containerBytes);
        if (container == NULL) {
            printf(LOGNAME ": no container at %s, so there is nothing at (%.1f, %.1f, %.1f)\n", xudPositioningPaths[index],
                (double)sources[index].position.x, (double)sources[index].position.y, (double)sources[index].position.z);
            xud_positioning_close_all(context, sources, font, fontFile);
            return 1;
        }
        {
            const XudAudioData audio = { .fileData = container, .fileBytes = containerBytes };
            step = xud_load_audio(&sources[index].handle, context, &audio);
        }
        free((void*)container);
        if (step != XUD_OK) {
            xud_text_error("xud_load_audio", step);
            xud_positioning_close_all(context, sources, font, fontFile);
            return 1;
        }
        step = xud_set_audio_source_position(context, sources[index].handle, sources[index].position);
        if (step != XUD_OK) {
            xud_text_error("xud_set_audio_source_position", step);
            break;
        }
        step = xud_play_audio(context, sources[index].handle);
        if (step != XUD_OK) {
            xud_text_error("xud_play_audio", step);
            break;
        }
        printf(LOGNAME ": %s at (%.1f, %.1f, %.1f) -> playing\n", xudPositioningNames[index], (double)sources[index].position.x,
            (double)sources[index].position.y, (double)sources[index].position.z);
    }

    if (step != XUD_OK) {
        xud_positioning_close_all(context, sources, font, fontFile);
        return 1;
    }

    /* The cursor goes before the walk, the way camera_first_person.c hides it: a first-person look
       reads pointer DELTAS, and ESC takes the arrow back on the way out.
    */
    if (xud_hide_cursor(context) != XUD_OK) {
        printf(LOGNAME ": xud_hide_cursor -> refused, so the look is the arrow keys' alone\n");
        cursorHidden = 0u;
    }

    for (;;) {
        bool shouldClose = false;
        bool pressed = false;
        XudVector3 facing = { .x = 0.0f, .y = 0.0f, .z = 0.0f };
        char line[XUD_POSITIONING_LINE_MAX];
        float facingLength = 0.0f;
        float frameTime = 0.0f;
        float nearestDistance = 0.0f;
        int written = 0;
        uint32_t playing = 0u;
        uint32_t nearest = 0u;

        step = xud_window_should_close(&shouldClose, context);
        if (step != XUD_OK) {
            xud_text_error("frame xud_window_should_close", step);
            break;
        }
        if (shouldClose) {
            printf(LOGNAME ": frame %u: the window was closed\n", frame);
            break;
        }
        if ((xud_is_key_pressed(&pressed, context, XUD_KEY_ESCAPE) == XUD_OK) && pressed) {
            printf(LOGNAME ": frame %u: ESC\n", frame);
            break;
        }

        step = xud_update_camera(context, &camera, XUD_CAMERA_MODE_FIRST_PERSON);
        if (step != XUD_OK) {
            xud_text_error("frame xud_update_camera", step);
            break;
        }

        /* The auto-walk, when the run was asked for one: the forward step and the turn a held W and
           a moved mouse produce, handed to the MANUAL half of the camera rung so that a run with no
           keyboard in front of it still moves the ear. Fixed speed, fixed turn, the frame's own span
           as its clock - the same path twice, and no random number in it.
        */
        if (xudPositioningAutoWalk != 0u) {
            if (xud_get_frame_time(&frameTime, context) != XUD_OK) {
                frameTime = 0.0f;
            }
            step = xud_update_camera_pro(&camera, (XudVector3) { .x = XUD_POSITIONING_WALK_SPEED * frameTime, .y = 0.0f, .z = 0.0f },
                (XudVector3) { .x = XUD_POSITIONING_WALK_TURN * frameTime, .y = 0.0f, .z = 0.0f }, 0.0f);
            if (step != XUD_OK) {
                xud_text_error("frame xud_update_camera_pro", step);
                break;
            }
        }

        /* The ear, moved once for the whole run's worth of sound: the eye's position and the eye's
           facing, handed over as the two context-level numbers they are. Each setter recomputes every
           spatial handle, so the three placed streams follow the walk without this loop writing to
           any of them again - the half raylib's example does with a four-call module function per
           sound per frame, because there the listener is glued to the sound. The facing goes first so
           that the position's recompute, the later of the two, is the one that carries both. A facing
           of zero length - the eye on its own target - has no left or right to derive, and is refused
           rather than guessed at; the last accepted facing then still stands.
        */
        facing.x = camera.target.x - camera.position.x;
        facing.y = camera.target.y - camera.position.y;
        facing.z = camera.target.z - camera.position.z;
        facingLength = sqrtf((facing.x * facing.x) + (facing.y * facing.y) + (facing.z * facing.z));
        if (facingLength > 0.0f) {
            step = xud_set_audio_listener_forward(context, facing);
            if ((step != XUD_OK) && (facingRefused == 0u)) {
                xud_text_error("xud_set_audio_listener_forward", step);
                facingRefused = 1u;
            }
        }
        step = xud_set_audio_listener_position(context, camera.position);
        if (step != XUD_OK) {
            xud_text_error("frame xud_set_audio_listener_position", step);
            break;
        }

        /* raylib's example restarts a source the moment it stops (its file, line 108), and so does
           this one: the recordings are a second long, the walk is not, and a run that let them end
           would leave an ear with nothing to walk around. The same walk picks the nearest playing
           source for the HUD's first row.
        */
        nearestDistance = 0.0f;
        for (index = 0u; index < XUD_POSITIONING_SOUND_COUNT; index++) {
            bool isPlaying = false;

            if (xud_is_audio_playing(&isPlaying, context, sources[index].handle) == XUD_OK) {
                if (!isPlaying) {
                    (void)xud_play_audio(context, sources[index].handle);
                } else {
                    const float distance = xud_positioning_distance(camera.position, sources[index].position);

                    if ((playing == 0u) || (distance < nearestDistance)) {
                        nearest = index;
                        nearestDistance = distance;
                    }
                    playing++;
                }
            }
        }

        /* The frame's draws. The camera is begun here and NOT ended inside the frame - see the
           Pairing half of the banner - and the HUD is screen-fixed, so the ear moves the scene
           around the text rather than the text around the scene.
        */
        step = xud_begin_drawing(context);
        if (step != XUD_OK) {
            xud_text_error("frame xud_begin_drawing", step);
            break;
        }
        step = xud_clear_background(context, xudPositioningRaywhite);
        if (step != XUD_OK) {
            xud_text_error("frame xud_clear_background", step);
            (void)xud_end_drawing(context);
            break;
        }
        step = xud_begin_camera_3d(context, camera);
        if (step != XUD_OK) {
            xud_text_error("frame xud_begin_camera_3d", step);
            (void)xud_end_drawing(context);
            break;
        }
        step = xud_positioning_scene(context, sources);
        if (step != XUD_OK) {
            xud_positioning_count_short_frame(frame, step, &shortFrames);
        }

        /* The HUD: the nearest thing the ear can hear on the first row, all three sources' own
           numbers below it, and the controls along the bottom. The distance and the pan are the
           example's reading of the geometry - see xud_positioning_pan - because the public getters
           answer the BASE the caller asked for and not the audible value, which is xud.h's stated
           choice rather than an accident of this file.
        */
        if (playing != 0u) {
            written = snprintf(line, sizeof(line), "HEARD %s  dist %5.2f  pan %+0.2f", xudPositioningNames[nearest],
                (double)nearestDistance, (double)xud_positioning_pan(&camera, sources[nearest].position));
        } else {
            written = snprintf(line, sizeof(line), "HEARD nothing playing");
        }
        if ((written > 0) && ((size_t)written < sizeof(line))) {
            step = xud_positioning_text(context, font, 10, 10, line, xudPositioningLime);
            if (step != XUD_OK) {
                xud_positioning_count_short_frame(frame, step, &shortFrames);
            }
        }
        for (index = 0u; index < XUD_POSITIONING_SOUND_COUNT; index++) {
            const float distance = xud_positioning_distance(camera.position, sources[index].position);

            written = snprintf(line, sizeof(line), "  %s at (%5.2f, %5.2f, %5.2f)  dist %5.2f  pan %+0.2f", xudPositioningNames[index],
                (double)sources[index].position.x, (double)sources[index].position.y, (double)sources[index].position.z, (double)distance,
                (double)xud_positioning_pan(&camera, sources[index].position));
            if ((written > 0) && ((size_t)written < sizeof(line))) {
                step = xud_positioning_text(context, font, 10, (int32_t)(10u + (20u * (index + 1u))), line, xudPositioningBlack);
                if (step != XUD_OK) {
                    xud_positioning_count_short_frame(frame, step, &shortFrames);
                }
            }
        }
        written = snprintf(
            line, sizeof(line), "WASD walk, mouse or arrows look, ESC quit - cursor %s", (cursorHidden != 0u) ? "hidden" : "shown");
        if ((written > 0) && ((size_t)written < sizeof(line))) {
            step = xud_positioning_text(context, font, 10, (int32_t)(XUD_POSITIONING_HEIGHT - 28u), line, xudPositioningBlack);
            if (step != XUD_OK) {
                xud_positioning_count_short_frame(frame, step, &shortFrames);
            }
        }

        step = xud_end_drawing(context);
        if (step != XUD_OK) {
            xud_text_error("frame xud_end_drawing", step);
            break;
        }
        frame++;
    }

    /* The camera goes away with the run rather than with the frame: the last frame it was live for
       has been submitted by now.
    */
    step = xud_end_camera_3d(context);
    if (step != XUD_OK) {
        xud_text_error("xud_end_camera_3d", step);
    }

    printf(LOGNAME ": %u frame(s), %u of them short, ear at (%.2f, %.2f, %.2f), cursor %s\n", frame, shortFrames, (double)camera.position.x,
        (double)camera.position.y, (double)camera.position.z, (cursorHidden != 0u) ? "hidden" : "shown");

    (void)xud_show_cursor(context);
    xud_positioning_close_all(context, sources, font, fontFile);

    return 0;
}

int main(int argc, char** argv) {
    XudContext* context = NULL;
    XudResult step;
    int status = 0;

    if ((argc > 1) && (strcmp(argv[1], "walk") == 0)) {
        xudPositioningAutoWalk = 1u;
        printf(LOGNAME ": auto-walk on, %.1f unit(s)/s forward and %.0f degree(s)/s of yaw\n", (double)XUD_POSITIONING_WALK_SPEED,
            (double)XUD_POSITIONING_WALK_TURN);
    }

    step = xud_init_context(&context, XUD_POSITIONING_WIDTH, XUD_POSITIONING_HEIGHT);
    printf(LOGNAME ": xud_init_context(%u, %u) -> %s (xudlibc %s)\n", XUD_POSITIONING_WIDTH, XUD_POSITIONING_HEIGHT, xud_result_name(step),
        XUD_VERSION);
    if (step != XUD_OK) {
        return (step == XUD_UNSUPPORTED) ? 0 : 1;
    }

    status = xud_run(context);

    step = xud_close_context(&context);
    printf(LOGNAME ": xud_close_context -> %s\n", xud_result_name(step));

    return (status != 0) ? status : ((step == XUD_OK) ? 0 : 1);
}
