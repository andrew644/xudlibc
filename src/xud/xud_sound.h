/***********************************************************************************************
 *
 *   xud_sound.h - the sound rung: miniaudio's engine (its ma_device, its node graph and its
 *                 mixer) above the audio rung's one host block, the one play node per AUDIO-
 *                 STREAM slot behind it, and the play/stop pair xud.h declares on top of it
 *
 *   Owns : XudSound (the engine, the row array and the row count) and XudSoundRow (the one
 *          node pair behind one AUDIOSTREAM slot) plus the rungs above it - xud_sound_result_
 *          from_ma(), xud_sound_open(), xud_sound_play(), xud_sound_stop(), xud_sound_pause(),
 *          xud_sound_resume(), xud_sound_is_playing(), xud_sound_detach() and the teardown rung
 *          xud_sound_close().
 *   Layer: above the audio rung and below the public API. This is the SECOND audio rung and it
 *          is the one the audio rung's own note says was missing: what landed there was every
 *          number a play row needs beside the device, and what lands here is the device, the
 *          graph and the mixer, so a stream's frames can finally be heard rather than counted.
 *
 *   Why the engine rather than a bare ma_device and a hand-built graph: miniaudio's own low-
 *   level shape (a ma_device, a ma_node_graph, one ma_data_source_node per stream) leaves the
 *   loop to the caller - the device's data callback has to be filled by whoever owns the graph,
 *   and the graph only advances while that callback runs. That is a thread of our own, a clock
 *   of our own and a mixer of our own, which is three things the repo would be inventing. The
 *   engine IS that shape (ma_engine's first member is its ma_node_graph, and ma_engine_init
 *   allocates the ma_device under it with ma_engine_data_callback_internal as its data callback
 *   and the mixer above both), so the rung spends one engine per context, starts the device in
 *   side it, and the graph advance is miniaudio's rather than this tree's. The engine's resour
 *   ce manager is allocated and never used: every node here is built from a data source the
 *   audio rung already decoded, so no file is opened below this rung and the path half of minia
 *   udio's sound API is not in this tree.
 *
 *   Why one node per AUDIOSTREAM slot rather than one node per play call: raylib's PlayAudio
 *   spends a fresh sound per call and answers its index, and this repo's currency is a handle
 *   with a generation in it, not an index a caller has to remember, so the pair here is keyed
 *   by the stream instead: play starts the stream's node and stop stops it, and a stream played
 *   twice reuses the one node rather than spending a second one that would have to be found
 *   again at close. The row array is indexed by the AUDIOSTREAM slot's own index for that re
 *   as on, and holds a pointer per index rather than a row for the reason the row's type note
 *   gives. The row is destroyed when the slot is released (xud_close_audio runs xud_sound_
 *   detach() before it frees the record) because a row whose stream is gone is a node reading
 *   a block that was just freed - which is also why that close refuses while the row is playing
 *   rather than freeing under it.
 *
 *   What this rung does NOT own, and why: the frames. The node's data source is a ma_audio_buf
 *   fer_ref OVER the audio record's block, not a copy of it, so the block has exactly one owner
 *   (the audio record) and the row only names it; a copy would be a second ceiling on the same
 *   stream and the audio rung's ceiling is stated once, in one place. Also absent: volume, pos
 *   ition and the footprint half of raylib's audio surface (SetAudioFootprint), which is a spa
 *   tializer question above this rung rather than a device question inside it, and the containe
 *   rs the vendored miniaudio still cannot decode (qoa, xm/mod), which is an ask to include th
 *   eir headers rather than a decoder to write here.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#ifndef XUD_XUD_SOUND_H_
#define XUD_XUD_SOUND_H_

#include "miniaudio.h" /* Required for: ma_engine, ma_sound, ma_audio_buffer_ref, ma_result, ma_format_f32 - the engine this rung spends and the node pair behind one row. Included again here on purpose: the implementation lives in xud_vendor_miniaudio.c, and this header is the caller's half of that split, the same way xud_audio.h includes it. */
#include "xud.h" /* Required for: XudResult, XudHandle, XUD_HANDLE_KIND_AUDIOSTREAM - the public surface this rung is the half of */
#include "xud_audio.h" /* Required for: XudAudioStreamRecord and xud_audio_get() - the frames this rung's nodes read, and the accessor that hands them over */
#include "xud_handle.h" /* Required for: XudHandleTable and the slot index that indexes the row array below */

#include <assert.h> /* Required for: static_assert, the bounds below against each other */
#include <stdbool.h> /* Required for: bool - the one answer xud_sound_is_playing() gives */
#include <stdint.h> /* Required for: uint32_t - the row count, the row capacity and every bound above */
#include <stdlib.h> /* Required for: calloc, free - the row array this rung spends */

/*-----------------------------------------------------------------------------------------------
 * The rung's own bounds, and the one bound that is not a new one
 *-----------------------------------------------------------------------------------------------

   The row array is indexed by an AUDIOSTREAM slot index, so its first step and its ceiling are
   the handle layer's own slot minimum and slot ceiling read through one row per slot instead of
   one row per kind: a row bound of this rung's own would be a bound a stream could be loaded
   under and refused above, which is the disagreement the handle layer's note about its two capa
   city bounds exists to prevent. The second assert is the half that has to hold on its own: the
   doubling sequence below has to land on the ceiling exactly, or the last row of the array is
   unreachable and a handle the table can hand out has no row to name.
*/

#define XUD_SOUND_ROW_MIN_CAPACITY                                                                                                         \
    XUD_HANDLE_SLOT_MIN_CAPACITY /* the handle layer's slot minimum: one row per AUDIOSTREAM slot index, so the same first step */
#define XUD_SOUND_ROW_MAX_CAPACITY                                                                                                         \
    XUD_HANDLE_SLOT_MAX_CAPACITY /* the handle layer's slot ceiling: a row array that could outlive it would be an array whose rows address slots the table cannot hand out */

static_assert(XUD_SOUND_ROW_MIN_CAPACITY < XUD_SOUND_ROW_MAX_CAPACITY,
    "xud_sound.h: the row array's first step has to be below its ceiling, or the doubling sequence below has no range to double through");
static_assert((XUD_SOUND_ROW_MAX_CAPACITY % XUD_SOUND_ROW_MIN_CAPACITY) == 0u,
    "xud_sound.h: the ceiling must be a whole multiple of the first step, or the doubling sequence steps past it and the last rows of the "
    "array are unreachable");

/*-----------------------------------------------------------------------------------------------
 * The row's four states, and why the pause names a hold rather than a position
 *-----------------------------------------------------------------------------------------------

   NONE and STOPPED are two states rather than one because the node behind a row costs a graph
   attach and a heap allocation, and a stream stopped and played again is the same stream: the
   node stays and only its state moves. PAUSED is the row the caller asked the mixer to hold while
   it was reading it: the stop the pause makes leaves the cursor where the node left it, and the
   cursor belongs to miniaudio's data source rather than to this rung, so the state names the hold
   and the resume takes the cursor from where the hold left it - the rung exposes no number it does
   not own, which is the reason the audio rung's note on frames gives.
*/

#define XUD_SOUND_ROW_STATE_NONE 0u /* no node was ever spent for this slot index, which is what a zero-initialised row is */
#define XUD_SOUND_ROW_STATE_STOPPED                                                                                                        \
    1u /* the node exists and is not playing: it stays attached, so the next play costs a state change rather than a new node */
#define XUD_SOUND_ROW_STATE_PLAYING 2u /* the node exists and the mixer is reading it */
#define XUD_SOUND_ROW_STATE_PAUSED                                                                                                         \
    3u /* the node exists, is not reading, and the resume takes the cursor from where the pause left it rather than from frame 0 */
#define XUD_SOUND_ROW_STATE_COUNT 4u /* one past the highest row state, which is the row count of the state table this rung's checks read */

static_assert(XUD_SOUND_ROW_STATE_NONE == 0u,
    "xud_sound.h: NONE must be the 0th state, which is what makes a row this rung calloc-ed name a node that was never spent");
static_assert(XUD_SOUND_ROW_STATE_COUNT > XUD_SOUND_ROW_STATE_PLAYING,
    "xud_sound.h: the count must be one past the highest state, or the walk below stops before the last state it can reach");

/*-----------------------------------------------------------------------------------------------
 * The row behind one AUDIOSTREAM slot, and the engine above it
 *-----------------------------------------------------------------------------------------------*/

typedef struct XudSoundRow {
    uint32_t
        state; /* XUD_SOUND_ROW_STATE_NONE when no node was ever spent for this slot, XUD_SOUND_ROW_STATE_STOPPED once one exists and is not playing, XUD_SOUND_ROW_STATE_PLAYING while it is, XUD_SOUND_ROW_STATE_PAUSED while the pause's hold on it is in place */
    ma_audio_buffer_ref
        bufferRef; /* a reference OVER the audio record's block, not a copy: the record owns the block and this row only names it */
    ma_sound sound; /* the engine node pair, attached to the engine's endpoint, reading the row's own bufferRef */
} XudSoundRow;

typedef struct XudSound {
    ma_engine
        engine; /* the node graph, the ma_device inside it and the mixer above both - EMBEDDED rather than pointed at, because nothing names an address inside the engine (a row attaches to the endpoint by reference, not by address), so the engine has one owner and one address for as long as this struct has one */
    XudSoundRow**
        rows; /* one POINTER per AUDIOSTREAM slot index: the endpoint's own input bus is a linked list whose items live inside a row's sound struct, so a row that moved would leave the graph naming the address it moved from - the array moves, the rows never do */
    uint32_t
        rowCount; /* the array's own high mark, which moves with every growth step and is what bounds the teardown walk rather than the capacity */
    uint32_t rowCapacity; /* rows of the array allocated, a step of the doubling sequence below */
    XudAudioListener
        listener; /* the ONE ear of this device: raylib attaches a listener to a sound and this library hands out handles over a context, so
     the listener belongs to the device rather than to a row, which is what makes one call per frame move every spatial row at once. The two
     vectors are the world origin and +z until a caller says otherwise - xud_audio_open() spends no device and xud_sound_open() is the first
     thing that could hold a listener, so the default is what a device that was never moved or turned answers */
    uint32_t liveRows; /* rows that own a node, which is what xud_sound_close() reports and walks */
} XudSound;

/*-----------------------------------------------------------------------------------------------
 * The pure half
 *-----------------------------------------------------------------------------------------------*/

/// Pure: miniaudio's answer turned into this library's code, beside the audio rung's own table an
/// d one row short of it: a decoder has no backend to ask for and a device does, so MA_NO_BACKEND, M
/// A_NO_DEVICE and MA_BACKEND_NOT_ENABLED are the one shape this rung can hit that the audio rung ne
/// ver can, and they answer XUD_UNSUPPORTED rather than a rung half-open. MA_OUT_OF_MEMORY is this l
/// ibrary's own code for the same reason the audio rung states it; MA_INVALID_ARGS, MA_INVALID_OPERA
/// TION, MA_OUT_OF_RANGE, MA_TOO_BIG, MA_NOT_IMPLEMENTED and MA_ERROR are this file's shape rather th
/// an a caller's, because this rung hands miniaudio a config it built itself. XUD_OK on MA_SUCCESS; X
/// UD_INTERNAL_ERROR on nothing else - the default is that code so an unhandled shape still refuses.
XudResult xud_sound_result_from_ma(ma_result result);

/*-----------------------------------------------------------------------------------------------
 * The bring-up half
 *-----------------------------------------------------------------------------------------------*/

/// Driver: the engine and the device inside it, in miniaudio's own order - ma_engine_init spends
/// the node graph, the ma_device under it (with the engine's own data callback, which is what adv
/// ances the graph) and the mixer above both - and then ma_device_start on the device the engine
/// allocated, because miniaudio opens every device asleep. This rung calls driver functions (the
/// ALSA, Pulse or Jack backend miniaudio dlopens), which is the one reason this is the first rung
/// of this tree whose open is not pure host work. XUD_INVALID_ARG on a NULL out-handle or on an
/// out-handle that was not NULL on entry; XUD_UNSUPPORTED when no backend this vendored miniaudio
/// can drive is present (MA_NO_BACKEND, MA_NO_DEVICE, MA_BACKEND_NOT_ENABLED); XUD_OUT_OF_MEMORY
/// when the engine or one of its allocations cannot be spent; XUD_INTERNAL_ERROR on miniaudio's
/// own shape this rung built rather than asked for. On every refusal nothing is left half-open.
XudResult xud_sound_open(XudSound** outSound);

/*-----------------------------------------------------------------------------------------------
 * The play/stop/pause/resume four, the one question and the release
 *-----------------------------------------------------------------------------------------------*/

/// Driver: one stream's node, spent on the first play of the slot and reused after that, then star
/// ted. Order: the stream validated against the AUDIOSTREAM kind through the audio rung's own acc
/// essor (so a stale handle, a cross-kind handle and a fabricated one refuse before a row is spent
/// ), the row array grown to name the slot's index if it does not yet, the node built from a ma_au
/// dio_buffer_ref OVER the record's block and attached to the engine's endpoint when the row has no
/// node yet, then the row's state settled against the node, then ma_sound_start. The settle is the
/// one place the row learns its stream reached the end: the node is the one thing the audio thread
/// moves, and miniaudio stops a node itself when its data source runs out, so a row that reached
/// the end is settled to STOPPED here, and the restart is miniaudio's own note that ma_sound_start
/// seeks back when the sound is at end. A play on a row stopped mid-way resumes where it stopped,
/// which is the other half of that same note that a stopped sound is not rewound, and a play on a
/// row held by a pause is the same resume from the hold for the same reason. XUD_INVALID_ARG on a
/// NULL sound rung or table, or on a handle that does not name a live AUDIOSTREAM slot of this
/// table; XUD_ALREADY when the row is playing for real (the state this call asks for is already the
/// state it has); XUD_OUT_OF_MEMORY when the row array or the node cannot be spent; XUD_UNSUPPORTED
/// on a stream whose block has no frames to read; XUD_INTERNAL_ERROR on the row bound this rung's own
/// ceiling cannot name.
XudResult xud_sound_play(XudSound* sound, XudHandleTable* table, XudHandle stream);

/// Driver: one stream's node stopped, which is a state change on the node that stays attached: the
/// frames go back to the audio record and the node goes back to the next play of the same slot. The
/// row's state is settled against the node first, for the reason the play half states, so a stream
/// that reached its end is stopped already by miniaudio and answers here the same as one stopped. XUD
/// _INVALID_ARG on a NULL sound rung or table, or on a handle that does not name a live AUDIOSTREAM
/// slot of this table; XUD_ALREADY when the slot never had a node or its node is already stopped (t
/// he state this call asks for is already the state it has); XUD_OK once the mixer has been told.
XudResult xud_sound_stop(XudSound* sound, XudHandleTable* table, XudHandle stream);

/// Driver: one stream's node told to hold, which is the stop the pause makes: the node stays
/// attached, the cursor stays where the node left it, inside miniaudio's data source, and the row
/// moves to PAUSED rather than STOPPED, so the resume knows to take the cursor from there rather
/// than from frame 0. The row's state is settled against the node first, for the reason the play
/// half states, so a stream that reached its end is stopped already by miniaudio and answers here
/// the same as one never played. XUD_INVALID_ARG on a NULL sound rung or table, or on a handle
/// that does not name a live AUDIOSTREAM slot of this table; XUD_ALREADY when the slot never had
/// a node, its node is already stopped, or the row is already held (the state this call asks for
/// is already the state it has); XUD_OK once the mixer has been told and the row is held.
XudResult xud_sound_pause(XudSound* sound, XudHandleTable* table, XudHandle stream);

/// Driver: one held stream's node started again, which is the play the resume makes: the row
/// moves back to PLAYING and takes the cursor from where the pause left it, so the frames that
/// were never read are the ones still to come rather than the ones already heard. A resume of a
/// row that was stopped rather than held is the play half's own business, so it answers ALREADY
/// here and the row stays where it is. The row's state is settled against the node first, for the
/// reason the play half states. XUD_INVALID_ARG on a NULL sound rung or table, or on a handle
/// that does not name a live AUDIOSTREAM slot of this table; XUD_ALREADY when the slot never had
/// a node, its node is stopped, or the row is already playing (the state this call asks for is
/// already the state it has); XUD_OK once the mixer has been told and the row is playing.
XudResult xud_sound_resume(XudSound* sound, XudHandleTable* table, XudHandle stream);

/// One question a caller can ask of a row - is the mixer reading it - answered off the row's own
/// state after settling it against the node: the row's state is the one thing the caller's thread
/// owns, the node is the one thing the audio thread moves, so the question is asked of both, and a
/// row that says playing while the node says otherwise is a row whose stream reached the end, which
/// the settle moves to STOPPED. The answer is therefore this rung's bookkeeping, kept true, rather
/// than a race against the thread that pulls frames. XUD_INVALID_ARG on a NULL out-playing or a NUL
/// L sound rung; XUD_OK with false on a slot that never had a node or on one whose node is stopped
/// - a stop or a stream that reached its end - which is the answer rather than an error because the
/// question is answerable either way.
XudResult xud_sound_is_playing(bool* outPlaying, XudSound* sound, XudHandleTable* table, XudHandle stream);

/// Driver: one row's level, written on the node the row owns - ma_sound_set_volume, the engine's
/// own per-node setter. A slot with no node is the state the call asks for already being the
/// state it has: the value the public surface stored in the record is the one a row takes when
/// xud_sound_play() builds it, so there is nothing here to apply it to and nothing to fail.
/// XUD_INVALID_ARG on a NULL sound rung or on a level outside the range the public surface states
/// (below the silence or above the block's own peak); XUD_OK when the row was told or had none.
XudResult xud_sound_set_row_volume(XudSound* sound, XudHandle stream, float volume);

/// Driver: one row's rate, written on the node the row owns - ma_sound_set_pitch, which the row's
/// sound was built with the pitch flag off so that this call is the one it takes. A slot with no
/// node is the state the call asks for already being the state it has, for the reason the volume
/// half states: the record's value is the one a row takes when it is built. XUD_INVALID_ARG on a
/// NULL sound rung or on a non-positive rate - the one ma_sound_set_pitch() would take and do
/// nothing with, which would leave the record and a live row naming different pitches; XUD_OK
/// when the row was told or had none.
XudResult xud_sound_set_row_pitch(XudSound* sound, XudHandle stream, float pitch);

/// Driver: one row's position, written on the node the row owns - ma_sound_set_pan, the panner
/// every engine node carries, which the row's sound runs after its spatialization for the reason
/// miniaudio's own panner note states: panning happens after spatialization so that a mono sound
/// pans too. A slot with no node is the state the call asks for already being the state it has,
/// for the reason the volume half states: the record's value is the one a row takes when it is
/// built. XUD_INVALID_ARG on a NULL sound rung or on a position outside the range the public
/// surface states (-1.0 the full left to 1.0 the full right); XUD_OK when the row was told or
/// had none.
XudResult xud_sound_set_row_pan(XudSound* sound, XudHandle stream, float pan);

/// Driver: one SPATIAL row's level and position, rewritten from the device's listener - the write
/// the public positional pair runs instead of the base it was handed, so a row of a spatial handle
/// never hears a value the geometry did not produce. Order: the stream's record read through the
/// audio rung's accessor, the base and the source position handed to xud_audio_spatial_gain() beside
/// this rung's own listener, then the two writes on the node the row owns IF it has one - a slot
/// with no node is the state the call asks for already being the state it has, for the reason the
/// volume half gives: the record keeps the base and xud_sound_play() takes the SPATIAL value from
/// it when it builds the row. A NON-spatial handle is not this call's business at all and answers
/// XUD_OK without a write. XUD_INVALID_ARG on a NULL sound rung or table, or on a handle that does
/// not name a live AUDIOSTREAM slot of this table; XUD_UNSUPPORTED on a listener this rung cannot
/// pan against (a forward parallel to the world's up, which the public setter refuses before it can
/// be stored, so this is this file's shape rather than a caller's); XUD_OK once the node was told or
/// had none.
XudResult xud_sound_apply_spatial(XudSound* sound, XudHandleTable* table, XudHandle stream);

/// Driver: every spatial row of this table rewritten, which is what a listener that MOVED owes the
/// device - raylib recomputes in the mixer's own pass and this rung has no mixer thread of its own
/// to borrow, so the recompute runs on the caller's thread inside the setter that moved the ear.
/// The walk is the audio rung's own: bounded by the AUDIOSTREAM array's high mark rather than its
/// capacity, skipping every slot with no record behind it and every record that is not spatial, so
/// a table of non-spatial streams costs the walk and nothing else. XUD_INVALID_ARG on a NULL sound
/// rung or table; XUD_OK with a count of the rows that were rewritten, one per spatial slot, on a
/// table whose audio array was never allocated the count zero.
XudResult xud_sound_apply_spatial_all(XudSound* sound, XudHandleTable* table, uint32_t* outApplied);

/// Driver: one row's node destroyed and its bufferRef reference given back, which is what the audio
/// rung's close runs BEFORE it frees the record the row reads: a node left attached over a block the
/// record is about to free is a mixer reading freed memory, so a row that is playing refuses rather
/// than being stopped silently on the caller's behalf. A row that reached its end is not playing:
/// miniaudio stopped its node, and the settle answers that before the refusal, so a close after the
/// stream ends is the same close as a close after a stop. XUD_INVALID_ARG on a NULL sound rung or ta
/// ble, or on a handle that does not name a live AUDIOSTREAM slot of this table; XUD_UNSUPPORTED whi
/// le the row is playing (the close is not supported over a row in use, and the caller's answer is t
/// o stop it first); XUD_OK when the row was destroyed or had never owned a node.
XudResult xud_sound_detach(XudSound* sound, XudHandleTable* table, XudHandle stream);

/// Teardown: every row's node uninitialized and every bufferRef reference given back, then the eng
/// ine gone (which stops and destroys the device inside it), then the row array freed and the rung
/// itself freed. Runs ABOVE xud_audio_close_all() for the reason the detach note gives - a row names
/// a block an audio record owns - and is XUD_ALREADY on a rung that was never opened, which is the
/// state a context that never played anything leaves behind. XUD_INVALID_ARG on a NULL out-handle;
/// XUD_OK with a count of zero on a rung with no rows.
XudResult xud_sound_close(XudSound** sound, uint32_t* outClosed);

#endif /* XUD_XUD_SOUND_H_ */
