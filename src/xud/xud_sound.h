/***********************************************************************************************
 *
 *   xud_sound.h - the sound rung: miniaudio's engine (its ma_device, its node graph and its
 *                 mixer) above the audio rung's one host block, the one play node per AUDIO-
 *                 STREAM slot behind it, and the play/stop pair xud.h declares on top of it
 *
 *   Owns : XudSound (the engine, the row array and the row count) and XudSoundRow (the one
 *          node pair behind one AUDIOSTREAM slot) plus the rungs above it - xud_sound_result_
 *          from_ma(), xud_sound_open(), xud_sound_play(), xud_sound_stop(), xud_sound_is_pla
 *          ying(), xud_sound_detach() and the teardown rung xud_sound_close().
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
 * The row's three states, and why there is no fourth
 *-----------------------------------------------------------------------------------------------

   NONE and STOPPED are two states rather than one because the node behind a row costs a graph
   attach and a heap allocation, and a stream stopped and played again is the same stream: the
   node stays and only its state moves. PAUSED is absent for the reason the audio rung's note on
   frames gives - a paused row is a row whose cursor is somewhere in the block, and the cursor
   belongs to miniaudio's data source rather than to this rung, so the rung that would expose it
   would be exposing a number it does not own.
*/

#define XUD_SOUND_ROW_STATE_NONE 0u /* no node was ever spent for this slot index, which is what a zero-initialised row is */
#define XUD_SOUND_ROW_STATE_STOPPED                                                                                                        \
    1u /* the node exists and is not playing: it stays attached, so the next play costs a state change rather than a new node */
#define XUD_SOUND_ROW_STATE_PLAYING 2u /* the node exists and the mixer is reading it */
#define XUD_SOUND_ROW_STATE_COUNT 3u /* one past the highest row state, which is the row count of the state table this rung's checks read */

static_assert(XUD_SOUND_ROW_STATE_NONE == 0u,
    "xud_sound.h: NONE must be the 0th state, which is what makes a row this rung calloc-ed name a node that was never spent");
static_assert(XUD_SOUND_ROW_STATE_COUNT > XUD_SOUND_ROW_STATE_PLAYING,
    "xud_sound.h: the count must be one past the highest state, or the walk below stops before the last state it can reach");

/*-----------------------------------------------------------------------------------------------
 * The row behind one AUDIOSTREAM slot, and the engine above it
 *-----------------------------------------------------------------------------------------------*/

typedef struct XudSoundRow {
    uint32_t
        state; /* XUD_SOUND_ROW_STATE_NONE when no node was ever spent for this slot, XUD_SOUND_ROW_STATE_STOPPED once one exists and is not playing, XUD_SOUND_ROW_STATE_PLAYING while it is */
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
 * The play/stop pair, the one question and the release
 *-----------------------------------------------------------------------------------------------*/

/// Driver: one stream's node, spent on the first play of the slot and reused after that, then star
/// ted. Order: the stream validated against the AUDIOSTREAM kind through the audio rung's own acc
/// essor (so a stale handle, a cross-kind handle and a fabricated one refuse before a row is spent
/// ), the row array grown to name the slot's index if it does not yet, the node built from a ma_au
/// dio_buffer_ref OVER the record's block and attached to the engine's endpoint when the row has no
/// node yet, then ma_sound_start. A play on a row that had reached the end restarts at frame 0, wh
/// ich is miniaudio's own note that ma_sound_start seeks back when the sound is at end; a play on a
/// row stopped mid-way resumes where it stopped, which is the other half of that same note that a
/// stopped sound is not rewound. XUD_INVALID_ARG on a NULL sound rung or table, or on a handle tha
/// t does not name a live AUDIOSTREAM slot of this table; XUD_ALREADY when the
/// row is already playing (the state this call asks for is already the state it has); XUD_OUT_OF_
/// MEMORY when the row array or the node cannot be spent; XUD_UNSUPPORTED on a stream whose block
/// has no frames to read; XUD_INTERNAL_ERROR on the row bound this rung's own ceiling cannot name.
XudResult xud_sound_play(XudSound* sound, XudHandleTable* table, XudHandle stream);

/// Driver: one stream's node stopped, which is a state change on the node that stays attached: the
/// frames go back to the audio record and the node goes back to the next play of the same slot. XUD
/// _INVALID_ARG on a NULL sound rung or table, or on a handle that does not name a live AUDIOSTREAM
/// slot of this table; XUD_ALREADY when the slot never had a node or its node is already stopped (t
/// he state this call asks for is already the state it has); XUD_OK once the mixer has been told.
XudResult xud_sound_stop(XudSound* sound, XudHandleTable* table, XudHandle stream);

/// Pure above the rung: the one question a caller can ask of a row - is the mixer reading it - read
/// off the row's own state rather than off miniaudio's clock, so the answer is this rung's bookkee
/// ping and not a race against the thread that pulls frames. XUD_INVALID_ARG on a NULL out-playing
/// or a NULL sound rung; XUD_OK with false on a slot that never had a node or on one whose node is
/// stopped, which is the answer rather than an error because the question is answerable either way.
XudResult xud_sound_is_playing(bool* outPlaying, const XudSound* sound, XudHandleTable* table, XudHandle stream);

/// Driver: one row's node destroyed and its bufferRef reference given back, which is what the audio
/// rung's close runs BEFORE it frees the record the row reads: a node left attached over a block the
/// record is about to free is a mixer reading freed memory, so a row that is playing refuses rather
/// than being stopped silently on the caller's behalf. XUD_INVALID_ARG on a NULL sound rung or table
/// , or on a handle that does not name a live AUDIOSTREAM slot of this table; XUD_UNSUPPORTED while
/// the row is playing (the close is not supported over a row in use, and the caller's answer is to
/// stop it first); XUD_OK when the row was destroyed or had never owned a node.
XudResult xud_sound_detach(XudSound* sound, XudHandleTable* table, XudHandle stream);

/// Teardown: every row's node uninitialized and every bufferRef reference given back, then the eng
/// ine gone (which stops and destroys the device inside it), then the row array freed and the rung
/// itself freed. Runs ABOVE xud_audio_close_all() for the reason the detach note gives - a row names
/// a block an audio record owns - and is XUD_ALREADY on a rung that was never opened, which is the
/// state a context that never played anything leaves behind. XUD_INVALID_ARG on a NULL out-handle;
/// XUD_OK with a count of zero on a rung with no rows.
XudResult xud_sound_close(XudSound** sound, uint32_t* outClosed);

#endif /* XUD_XUD_SOUND_H_ */
