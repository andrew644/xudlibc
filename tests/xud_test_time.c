/***********************************************************************************************
 *
 *   xud_test_time.c - the clock rung: GLFW's timer read against one context's own epoch, and the
 *   public clock API above it
 *
 *   Owns : the saturating tick difference and the wrap it refuses, the seconds division and its
 * zero-frequency refusal, the two mark rows and the boundary count, the "no completed frame yet"
 * answer, the rung's own open/close guards and the window branch they reach - plus the GPU half th
 * at opens a real context, runs real frames through the frame pair and reads a real clock back. La
 * yer: tests. Sees xud_time.h (the rung), xud.h (the public calls) and xud_core.h (the shell the p
 * ublic calls reach the rung through), and xud_window.h for the synthetic window whose GLFW windo
 * w is NULL.
 *
 *   The pure half is the point of this area: xud_time_mark(), xud_time_ticks_diff() and xud_time_se
 * conds_from_ticks() take numbers rather than a window, so the whole of the arithmetic - the satur
 * ation a clock the environment can move costs, the division and the frequency that refuses it - i
 * s asserted with no window system and no driver present. What the headless half CANNOT reach is a
 * real tick: xud_time_elapsed() and the frame-span query above it read GLFW, and GLFW's timer answe
 * rs nothing until glfwInit has run, so those two are asserted against a real context in the GPU h
 * alf only.
 *
 *   Deterministic: hand-written ticks, fixed frame counts, and no wall time read in the headless h
 * alf at all - the checks that DO read a real clock in the GPU half are monotonicity and sign check
 * s, which hold whatever the clock says.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud.h" /* Required for: XudResult, the public clock calls, the frame pair the GPU half runs */
#include "xud_core.h" /* Required for: struct XudContext, whose time field the public calls reach */
#include "xud_test_harness.h" /* Required for: xud_check, xud_gpu_tests_enabled, the area entry points */
#include "xud_time.h" /* Required for: XudTime, the rung's rows and the halves the checks below drive */
#include "xud_window.h" /* Required for: XudWindow, the synthetic window whose glfwWindow field is NULL */
#include <stdlib.h> /* Required for: calloc and free, the synthetic rung, shell and window */

static void xud_test_time_arithmetic(void) {
    uint64_t diff = 0u;
    double seconds = 0.0;

    /* The difference of two GLFW ticks, in the three cases that exist: later above earlier, the sam
       e tick twice, and the later read BELOW the earlier one - which is the case the vendored tree'
       s CLOCK_REALTIME timer makes real (posix_time.c 46-50, and the build's -std=c11 -D_DEFAULT_SO
       URCE flags leave _POSIX_MONOTONIC_CLOCK undefined), and the case a uint64 difference turns in
       to a huge frame rather than a refused one.
    */
    xud_check(xud_time_ticks_diff(&diff, 2000u, 1000u) == XUD_OK, "time/diff-forward-ok", "a later read minus an earlier one is a span");
    xud_check(diff == 1000u, "time/diff-forward-value", "and the span is the difference");
    xud_check(xud_time_ticks_diff(&diff, 1000u, 1000u) == XUD_OK, "time/diff-equal-ok", "two reads of one tick are a span, not a refusal");
    xud_check(diff == 0u, "time/diff-equal-is-zero", "and that span is zero");
    xud_check(xud_time_ticks_diff(&diff, 500u, 1000u) == XUD_OK, "time/diff-backwards-ok", "a clock that moved backwards is not a refusal");
    xud_check(diff == 0u, "time/diff-saturates", "and the span saturates at zero rather than wrapping");

    /* The wrap this rung refuses is stated rather than assumed: the same pair subtracted in uint64
       arithmetic is a huge number, which is exactly the huge frame the note above says a caller wou
       ld pace a game with.
    */
    xud_check((uint64_t)(500u - 1000u) > 1000000000ull, "time/diff-wrap-was-huge",
        "the difference this rung refuses would have read as over a second of frame");
    xud_check(xud_time_ticks_diff(NULL, 2000u, 1000u) == XUD_INVALID_ARG, "time/diff-null-out-refused",
        "an out-span that is not there is refused");

    /* The division, at the POSIX timer's own frequency (posix_time.c 46-50 records 1e9 ticks a seco
       nd) - read rather than assumed by the query above this half, and checked here as the divisor
       the arithmetic takes.
    */
    xud_check(xud_time_seconds_from_ticks(&seconds, 1000000000ull, 1000000000ull) == XUD_OK, "time/seconds-one-ok",
        "one frequency of ticks is a span");
    xud_check(seconds == 1.0, "time/seconds-one-second", "and that span is one second");
    xud_check(xud_time_seconds_from_ticks(&seconds, 500000000ull, 1000000000ull) == XUD_OK, "time/seconds-half-ok",
        "half a frequency of ticks is a span");
    xud_check(seconds == 0.5, "time/seconds-half-second", "and that span is half a second");
    xud_check(
        xud_time_seconds_from_ticks(&seconds, 0u, 1000000000ull) == XUD_OK, "time/seconds-zero-ok", "no ticks is a span, not a refusal");
    xud_check(seconds == 0.0, "time/seconds-zero-second", "and that span is zero");
    xud_check(xud_time_seconds_from_ticks(&seconds, 1000u, 0u) == XUD_UNSUPPORTED, "time/seconds-zero-frequency-refused",
        "GLFW's answer for a library it never initialised is refused, not divided by");
    xud_check(seconds == 0.0, "time/seconds-refusal-clears-out", "and the refusal leaves zero rather than the previous answer");
    xud_check(xud_time_seconds_from_ticks(NULL, 1000u, 1000000000ull) == XUD_INVALID_ARG, "time/seconds-null-out-refused",
        "an out-seconds that is not there is refused");
}

static void xud_test_time_rows(void) {
    XudTime* time = NULL;
    double seconds = 0.0;

    time = calloc(1, sizeof(XudTime));
    xud_check(time != NULL, "time/synthetic-alloc", "the rung must allocate");
    if (time == NULL) {
        return;
    }

    /* The opening state, read through the query above the rows: no boundary has been marked, so the
       frame that completed most recently does not exist yet, and zero is that answer rather than a
       refusal - the reason a first frame's step is zero rather than garbage. This is also the one
       frame-span check the headless run can make: the answer needs no clock, so it is taken before
       any boundary exists.
    */
    xud_check(xud_time_frame_time(&seconds, time) == XUD_OK, "time/no-frame-yet-ok", "no completed frame is an answer, not a refusal");
    xud_check(seconds == 0.0, "time/no-frame-yet-zero", "and it is zero");

    xud_check(xud_time_mark(time, 1000u) == XUD_OK, "time/mark-first-ok", "one boundary is a boundary, not a span");
    xud_check(time->boundaries == 1u, "time/first-boundary-counted", "and it is counted");
    xud_check(time->markTick == 1000u, "time/first-boundary-is-the-mark", "the mark row is the boundary that opened this frame");
    xud_check(time->previousMarkTick == 0u, "time/first-boundary-has-no-row-below",
        "the row below it is still the sentinel this rung opens with");
    xud_check(xud_time_frame_time(&seconds, time) == XUD_OK, "time/one-boundary-still-zero", "one boundary is still no completed frame");
    xud_check(seconds == 0.0, "time/one-boundary-is-zero", "and the answer is still zero, not the tick itself");

    xud_check(xud_time_mark(time, 1500u) == XUD_OK, "time/mark-second-ok", "the boundary below this one is kept, not overwritten");
    xud_check(time->boundaries == 2u, "time/second-boundary-counted", "and the count says two");
    xud_check(time->markTick == 1500u, "time/second-boundary-is-the-mark", "this boundary is the mark row");
    xud_check(time->previousMarkTick == 1000u, "time/first-boundary-moved-down", "the one above it moved to the row below");

    /* A backwards boundary: the rows take it as it comes, because the saturation is the difference'
       s rule rather than a rewrite of the rows - which is what keeps one rule in one place.
    */
    xud_check(xud_time_mark(time, 400u) == XUD_OK, "time/mark-backwards-ok", "the rows take the tick as the clock answered it");
    xud_check(time->markTick == 400u, "time/backwards-boundary-is-the-mark", "and the mark row is what the clock said");
    xud_check(xud_time_mark(NULL, 1u) == XUD_INVALID_ARG, "time/mark-null-rung-refused", "a rung that is not there is refused");

    free(time);
}

static void xud_test_time_guards(void) {
    XudTime* time = NULL;
    XudContext* context = NULL;
    XudWindow* syntheticWindow = NULL;
    double seconds = 0.0;
    float frameSeconds = 0.0f;
    uint64_t ticks = 0u;

    /* The window branch of the open's guard, reachable with no window system: a window with no GLFW
       window behind it is the state xud_window_open() leaves when GLFW refused, and GLFW's timer has
       nothing to answer for it - so the answer is XUD_UNSUPPORTED rather than a record whose epoch
       is GLFW's zero-for-uninitialised answer.
    */
    syntheticWindow = calloc(1, sizeof(XudWindow));
    xud_check(syntheticWindow != NULL, "time/synthetic-window", "the synthetic window must allocate");
    if (syntheticWindow != NULL) {
        xud_check(xud_time_open(&time, syntheticWindow) == XUD_UNSUPPORTED, "time/open-no-glfw-window-refused",
            "a window with no GLFW window behind it has no timer");
        xud_check(time == NULL, "time/open-refusal-leaves-nothing", "and the refusal hands back no record");
    }
    xud_check(
        xud_time_open(&time, NULL) == XUD_INVALID_ARG, "time/open-null-window-refused", "a window that is not there is a bad argument");
    xud_check(
        xud_time_open(NULL, syntheticWindow) == XUD_INVALID_ARG, "time/open-null-out-refused", "an out-param that is not there is refused");
    free(syntheticWindow);

    /* The close's shape: XUD_ALREADY on a field that was never filled, which is what lets xud_close
       _context() run the same ladder over a context rejected part-way through bring-up, and a rung
       closed twice is freed once.
    */
    xud_check(xud_time_close(NULL) == XUD_INVALID_ARG, "time/close-null-out-refused", "an out-param that is not there is refused");
    xud_check(xud_time_close(&time) == XUD_ALREADY, "time/close-never-opened-already", "a field that was never filled is already closed");
    xud_check(time == NULL, "time/close-never-opened-clears", "and it stays clear");

    time = calloc(1, sizeof(XudTime));
    xud_check(time != NULL, "time/synthetic-rung", "the rung must allocate");
    if (time == NULL) {
        return;
    }
    xud_check(xud_time_close(&time) == XUD_OK, "time/close-opened-ok", "a rung that was there is freed");
    xud_check(time == NULL, "time/close-clears-the-handle", "and the handle is cleared, so a second close cannot double-free");

    /* The public guard, headless: a context with no clock rung is a context that never completed bri
       ng-up, and the answer is XUD_UNSUPPORTED without a GLFW call - which is the branch the GPU h
       alf below cannot reach.
    */
    context = calloc(1, sizeof(XudContext));
    xud_check(context != NULL, "time/synthetic-shell", "the shell must allocate");
    if (context != NULL) {
        xud_check(xud_get_time(&seconds, context) == XUD_UNSUPPORTED, "time/public-no-rung-unsupported",
            "no clock rung is a bring-up that never completed");
        xud_check(seconds == 0.0, "time/public-no-rung-clears", "and the refusal leaves zero behind");
        xud_check(xud_get_frame_time(&frameSeconds, context) == XUD_UNSUPPORTED, "time/public-frame-no-rung-unsupported",
            "the frame-span query is guarded the same way");
        xud_check(frameSeconds == 0.0f, "time/public-frame-no-rung-clears", "and it clears its own out-param");
        xud_check(xud_get_timer_value(&ticks, context) == XUD_UNSUPPORTED, "time/public-ticks-no-rung-unsupported",
            "so does the raw passthrough above GLFW");
        xud_check(ticks == 0u, "time/public-ticks-no-rung-clears", "and clears its out-param");
        xud_check(xud_get_timer_frequency(&ticks, context) == XUD_UNSUPPORTED, "time/public-frequency-no-rung-unsupported",
            "and the frequency beside it");
        xud_check(ticks == 0u, "time/public-frequency-no-rung-clears", "and that one too");
        xud_check(
            xud_get_time(NULL, context) == XUD_INVALID_ARG, "time/public-null-out-refused", "an out-param that is not there is refused");
        xud_check(
            xud_get_time(&seconds, NULL) == XUD_INVALID_ARG, "time/public-null-context-refused", "a context that is not there is refused");
        free(context);
    }
}

static void xud_test_time_gpu(void) {
    XudContext* context = NULL;
    XudTime* second = NULL;
    double before = 0.0;
    double after = 0.0;
    float frameSeconds = 0.0f;
    uint64_t ticksBefore = 0u;
    uint64_t ticksAfter = 0u;
    uint64_t frequency = 0u;

    xud_check(xud_init_context(&context, 64u, 64u) == XUD_OK, "time/gpu-context", "bring-up opened the clock rung as part of it");
    if (context == NULL) {
        return;
    }
    xud_check(context->time != NULL, "time/gpu-rung-exists", "the clock rung is a bring-up requirement, not an optional field");

    /* A real clock, read through the public surface: the frequency GLFW's own timer answers, and the
       elapsed against THIS context's epoch - which is a per-context question, so the first answer a
       fresh context gets is a small one rather than the age of the whole test process.
    */
    xud_check(xud_get_timer_frequency(&frequency, context) == XUD_OK, "time/gpu-frequency-ok", "a live GLFW answers a frequency");
    xud_check(frequency > 0u, "time/gpu-frequency-nonzero", "and it is a real divisor, not GLFW's zero-for-uninitialised answer");

    xud_check(xud_get_time(&before, context) == XUD_OK, "time/gpu-elapsed-ok", "the elapsed read against this context's own epoch");
    xud_check(before >= 0.0, "time/gpu-elapsed-non-negative", "the epoch is this context's own, so the elapsed cannot be negative");
    xud_check(before < 60.0, "time/gpu-elapsed-is-contexts-own", "a fresh context is seconds old, not as old as GLFW has been initialised");

    xud_check(xud_get_timer_value(&ticksBefore, context) == XUD_OK, "time/gpu-ticks-ok", "the raw tick passthrough read back");
    xud_check(ticksBefore > 0u, "time/gpu-ticks-nonzero", "a live GLFW answers ticks, and zero is its answer for being uninitialised");

    /* Three real frames through the frame pair, which is what marks three boundaries: the span the
       query below answers about is the span between the last two of them, and it covers the whole b
       oundary - the draw, the input poll and any rebuild that boundary ran.
    */
    for (uint32_t frame = 0u; frame < 3u; frame++) {
        xud_check(xud_begin_drawing(context) == XUD_OK, "time/gpu-begin-ok", "the frame pair opened a frame");
        xud_check(xud_end_drawing(context) == XUD_OK, "time/gpu-end-ok", "and closed it, which is where the boundary is marked");
    }

    xud_check(xud_get_frame_time(&frameSeconds, context) == XUD_OK, "time/gpu-frame-time-ok", "the span of a real frame read back");
    xud_check(frameSeconds > 0.0f, "time/gpu-frame-time-positive", "three real frames completed, so the span is not the no-frame answer");
    xud_check(frameSeconds < 1.0f, "time/gpu-frame-time-is-a-frame", "one frame of this run is well under a second");

    xud_check(xud_get_timer_value(&ticksAfter, context) == XUD_OK, "time/gpu-ticks-again-ok", "the passthrough read again");
    xud_check(ticksAfter >= ticksBefore, "time/gpu-ticks-monotonic", "GLFW's tick counter does not go backwards across frames");
    xud_check(xud_get_time(&after, context) == XUD_OK, "time/gpu-elapsed-again-ok", "the elapsed read again");
    xud_check(after >= before, "time/gpu-elapsed-monotonic", "and it does not go backwards either");
    xud_check(after > before, "time/gpu-elapsed-moved", "three frames of a real window moved the clock");
    xud_check((double)frameSeconds <= after, "time/gpu-span-within-elapsed",
        "one frame's span fits inside the elapsed since this context came up");

    /* The clock rung takes no window slot: unlike the input rung, which owns GLFW's payload slot for
       that window, this rung only reads process-level state, so a second rung above the same window
       is not a conflict and closes back with nothing of the driver's left behind.
    */
    xud_check(xud_time_open(&second, context->window) == XUD_OK, "time/gpu-second-rung-allowed",
        "the clock rung takes no window slot, so a second is not a conflict");
    xud_check(second != NULL, "time/gpu-second-rung-there", "and it came back");
    xud_check(xud_time_close(&second) == XUD_OK, "time/gpu-second-rung-closed", "and it closed, calling nothing of the driver's");
    xud_check(second == NULL, "time/gpu-second-rung-cleared", "and the handle is cleared");

    xud_check(xud_close_context(&context) == XUD_OK, "time/gpu-close-ok", "the clock rung's close is one free in the ladder");
    xud_check(context == NULL, "time/gpu-context-closed", "and the teardown cleared it");
}

void xud_test_time(void) {
    xud_test_time_arithmetic();
    xud_test_time_rows();
    xud_test_time_guards();
    if (xud_gpu_tests_enabled()) {
        xud_test_time_gpu();
    }
}
