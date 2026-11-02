/***********************************************************************************************
 *
 *   xud_time.h - the clock rung: GLFW's own timer above one context, and the elapsed and the
 *                frame-span halves the public clock API reads out of it
 *
 *   Owns : XudTime - the tick GLFW's timer read at this rung's open (the origin this context's
 *          elapsed is read against), the tick at the boundary that opened this frame and the one
 *          at the boundary below it (the pair a frame's span is a difference of) and how many bou
 *          ndaries have been marked - plus the rungs that bring it up and down (xud_time_open(),
 *          xud_time_close()), the one the frame pair runs at every boundary (xud_time_tick()) and
 *          the halves every answer is made of (xud_time_mark(), xud_time_ticks_diff(), xud_time_se
 *          conds_from_ticks(), xud_time_elapsed(), xud_time_frame_time()).
 *   Layer: above the window (it reads GLFW's process-level timer and nothing else - no Vulkan s
 *          ymbol, no loader row, no Vk anything) and below xud.h's clock calls, which are these
 *          records read through the one context guard. Beside xud_window.h and xud_input.h it is
 *          the third place in this tree that calls GLFW at all, and the only one of the three th
 *          at calls nothing but GLFW's timer.
 *
 *   Why the epoch is this rung's tick and not GLFW's own offset: GLFW's timer offset is process-l
 *          evel state (init.c 424, one _glfw.timer.offset for the whole library) and glfwInit() i
 *          s idempotent (init.c 386-389, the same fact xud_window.h's own note records), so glF
 *          WGetTime() answers "seconds since the FIRST window this process opened" rather than "se
 *          conds since this context came up" - two contexts in one process would read one clock
 *          rather than two, and a caller that closed a context and opened the next one would be h
 *          anded a clock that never restarted. raylib's GetTime is "elapsed time in seconds since
 *          InitWindow()", a per-window question, so this rung records the tick its own bring-up re
 *          ad and answers the difference against it. The two raw names below it are the pass-thro
 *          ugh GLFW's own two functions answer, unchanged: glfwGetTimerValue() and glfwGetTimerFr
 *          equency().
 *
 *   Why the span saturates rather than wraps: the vendored tree's POSIX timer is CLOCK_REALTIME u
 *          nless _POSIX_MONOTONIC_CLOCK is defined (posix_time.c 46-50), and the vendored build i
 *          s compiled with -std=c11 -D_DEFAULT_SOURCE (CMakeLists.txt), which leaves that macro u
 *          ndefined on this libc - so the environment, not GLFW, is free to move this clock: an N
 *          TP step or a date change makes a later read come back BELOW an earlier one. A uint64 d
 *          ifference of that pair is a huge number, and a huge frame span is a lie a caller would
 *          pace a game with. The difference therefore saturates at 0, and the note beside the publ
 *          ic query states what that costs: a caller that subtracts two xud_get_time() answers of
 *          its own can still see a negative delta, and that is the caller's to handle.
 *
 *   What this rung does NOT own, and why: glfwSetTime (it moves GLFW's process-level offset, so i
 *   t would move every context's origin at once - a knob with no honest meaning above a per-cont
 *   ext clock), and raylib's SetTargetFPS / GetFPS / WaitTime (a cap, a statistic and a sleep: al
 *   l three are decisions about the frame loop rather than questions about the clock, and the fra
 *   me loop above this rung is xud_core.c's).
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#ifndef XUD_XUD_TIME_H_
#define XUD_XUD_TIME_H_

#include "xud.h" /* Required for: XudResult - the codes every rung here answers with */
#include "xud_window.h" /* Required for: XudWindow and its GLFWwindow* field - the window whose existence is this rung's precondition, and, through it, <vulkan/vulkan.h> seen before <GLFW/glfw3.h> */
#include <GLFW/glfw3.h> /* LAST of the block: its Vulkan declarations sit inside #if defined(VK_VERSION_1_0), so xud_window.h above has already seen <vulkan/vulkan.h> - the include-order rule that header states. Included again here on purpose: this rung's driver halves ARE GLFW's two timer reads. */
#include <assert.h> /* Required for: static_assert, the two width checks below */
#include <stdint.h> /* Required for: uint64_t - GLFW's own tick type */

/*-----------------------------------------------------------------------------------------------
 * Two facts this rung's contract rests on, both compile-time
 *-----------------------------------------------------------------------------------------------*/

static_assert(sizeof(uint64_t) == 8u,
    "xud_time.h: glfwGetTimerValue and glfwGetTimerFrequency answer uint64_t (input.c 1499-1517), so the tick rows this rung keeps are "
    "eight bytes wide");
static_assert(sizeof(double) == 8u,
    "xud_time.h: glfwGetTime's own answer type is a double (input.c 1487), so the seconds this rung hands back are the same width GLFW's "
    "own answer is, which is what the 2^53 rounding note below states against");

/*-----------------------------------------------------------------------------------------------
 * The rung
 *-----------------------------------------------------------------------------------------------*/

typedef struct XudTime {
    uint64_t epochTick; /* GLFW's tick at this rung's open: the origin this context's elapsed is read against */
    uint64_t markTick; /* the tick read at the boundary that opened this frame */
    uint64_t previousMarkTick; /* the tick read at the boundary below it */
    uint64_t boundaries; /* boundaries marked since this rung opened: below 2 there is no span to answer */
} XudTime;

/*-----------------------------------------------------------------------------------------------
 * The rungs
 *-----------------------------------------------------------------------------------------------*/

/// Bring the rung up above a live window: one host allocation, GLFW's frequency asked once (GLFW a
/// swers 0 for it until glfwInit has run, and a window exists only after glfwInit has run - xud_win
/// dow.h's own note), so a frequency of 0 is the honest "no timer behind this window" refusal rather
/// than a division by zero left to the queries below. The epoch is GLFW's tick read HERE, which is w
/// hat makes the elapsed a per-context question rather than GLFW's process-level one (see the note a
/// bove). XUD_INVALID_ARG on a NULL out-param, one that already owns a rung or a NULL window; XUD_UNSUPPORTED on a
/// window with no GLFW window behind it, or on one whose timer GLFW cannot answer for; XUD_OUT_OF_MEMORY when the record cannot be
/// allocated.
XudResult xud_time_open(XudTime** outTime, XudWindow* window);

/// One frame boundary: GLFW's tick read and handed to the pure half below. This is the only call her
/// e that reads GLFW at a boundary, and the only reason xud_core.c's frame pair can stay free of an
/// y GLFW symbol. XUD_INVALID_ARG on a NULL rung; the pure half's codes otherwise.
XudResult xud_time_tick(XudTime* time);

/// The pure half above a tick handed over rather than read: move the two mark rows up one boundary a
/// nd count it. Taking the tick as a parameter is what makes the span arithmetic checkable with no w
/// indow system and no driver present (tests/xud_test_time.c drives it with hand-written ticks). XUD
/// _INVALID_ARG on a NULL rung; XUD_OK otherwise.
XudResult xud_time_mark(XudTime* time, uint64_t tick);

/// The saturating difference of two GLFW ticks: the span between an earlier and a later read, and 0
/// when the later read came back BELOW the earlier one - which is what a clock the environment can
/// move costs a uint64 difference, and a wrapped one is a huge frame rather than a refused one (see
/// the note above). XUD_INVALID_ARG on a NULL out-span; XUD_OK otherwise.
XudResult xud_time_ticks_diff(uint64_t* outDiff, uint64_t later, uint64_t earlier);

/// The seconds a tick span is worth at GLFW's own frequency: the same cast and the same division inp
/// ut.c 1490 runs over the same subtraction, so this rung's answer is GLFW's own rather than a re-de
/// rivation of it. A frequency of 0 is GLFW's answer for a library it never initialised, and it is r
/// efused as XUD_UNSUPPORTED rather than turned into a division by zero. Past 2^53 ticks the cast ro
/// unds - at the POSIX timer's 1e9 ticks a second that is about a century of one window staying open
/// , and GLFW's own division rounds at the same place. XUD_INVALID_ARG on a NULL out-seconds; XUD_UN
/// SUPPORTED on a frequency of 0; XUD_OK otherwise.
XudResult xud_time_seconds_from_ticks(double* outSeconds, uint64_t tickCount, uint64_t frequency);

/// Seconds since this rung's own epoch: GLFW's tick read now, the saturating difference against the
/// epoch, the division. Live, like raylib's GetTime - two calls in one frame answer two different nu
/// mbers, and the one that does not move across a frame is xud_time_frame_time() below. XUD_INVALID_A
/// RG on a NULL out-seconds or rung; XUD_UNSUPPORTED on a frequency GLFW cannot answer; XUD_OK other
/// wise.
XudResult xud_time_elapsed(double* outSeconds, const XudTime* time);

/// The span of the frame that completed most recently, in seconds: the two mark rows' saturating dif
/// ference, divided. Before the second boundary there is no completed frame to answer about, and 0.0
/// is that answer rather than a refusal - a caller that asks before the frame pair has run one bound
/// ary gets zero, which is what makes a first frame's step zero rather than garbage. XUD_INVALID_ARG
/// on a NULL out-seconds or rung; XUD_UNSUPPORTED on a frequency GLFW cannot answer; XUD_OK otherwise
/// .
XudResult xud_time_frame_time(double* outSeconds, const XudTime* time);

/// Tear the rung down: one free, and nothing of the driver's - GLFW's timer is process-level state th
/// is rung only reads, so there is nothing here to hand back. XUD_ALREADY on a NULL field, XUD_INVAL
/// ID_ARG on a NULL out-param, and the window this rung was opened above is never touched.
XudResult xud_time_close(XudTime** time);

#endif /* XUD_XUD_TIME_H_ */
