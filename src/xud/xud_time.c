/***********************************************************************************************
 *
 *   xud_time.c - the clock rung: GLFW's timer above one context, and the public clock calls above
 *                it
 *
 *   Owns : the rungs xud_time.h declares (xud_time_open(), xud_time_tick(), the pure xud_time_mark
 *          (), the two arithmetic halves, the two query halves and xud_time_close()) and the four p
 *          ublic clock calls xud.h declares above them, which are those records read through the one
 *          context guard. The split is the point of the file, and it is the input rung's split read
 *          one layer up: xud_time_tick() is the only call here that reads GLFW at a boundary, and
 *          everything the ticks MEAN - the span, the saturation, the division - is xud_time_mark()
 *          and xud_time_ticks_diff() and xud_time_seconds_from_ticks(), which take numbers rather
 *          than a window and can therefore be checked with no window system and no driver present.
 *          The public queries above this rung are the same records read through a context, so the
 *          headless half of this area asserts the arithmetic and the guards and only the GPU half a
 *          sserts a real clock.
 *
 *   What the rung owns and what it borrows: it owns one host allocation and the four rows inside it.
 *   It borrows the window - and only its existence: GLFW's timer is process-level state this rung r
 *   eads, never state it hands back, so xud_time_close() calls nothing at all and the window layer's
 *   own teardown is the thing that takes GLFW away.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include "xud_time.h" /* Required for: XudTime, the rung's own constants and the declarations this file answers */
#include "xud.h" /* Required for: XudResult and the public clock calls this file defines */
#include "xud_core.h" /* Required for: struct XudContext, whose time field the public calls reach */
#include "xud_window.h" /* Required for: XudWindow, the window whose GLFWwindow* is this rung's precondition */
#include <GLFW/glfw3.h> /* Required for: glfwGetTimerValue and glfwGetTimerFrequency - GLFW's whole timer surface, and the only GLFW names this file reads */
#include <stdlib.h> /* Required for: calloc and free, the one record this rung owns */

/*-----------------------------------------------------------------------------------------------
 * The rung
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_time_open(XudTime** outTime, XudWindow* window) {
    XudTime* time = NULL;

    if ((outTime == NULL) || (*outTime != NULL) || (window == NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((window == NULL) || (window->glfwWindow == NULL)) {
        return XUD_UNSUPPORTED;
    }

    time = calloc(1, sizeof(XudTime));
    if (time == NULL) {
        return XUD_OUT_OF_MEMORY;
    }

    /* GLFW's frequency is the honest "is there a timer behind this window" probe: glfwGetTimerFrequency
       answers 0 until glfwInit has run (_GLFW_REQUIRE_INIT_OR_RETURN(0), input.c 1514), and a window
       exists only after glfwInit has run, so a 0 here is a window system that refused GLFW rather th
       an a window whose clock this rung cannot divide by. Refusing is also what keeps every division
       below this rung from being a division by zero on a machine with no clock.
    */
    if (glfwGetTimerFrequency() == 0u) {
        free(time);
        return XUD_UNSUPPORTED;
    }

    time->epochTick = glfwGetTimerValue();
    *outTime = time;
    return XUD_OK;
}

XudResult xud_time_tick(XudTime* time) {
    if (time == NULL) {
        return XUD_INVALID_ARG;
    }
    return xud_time_mark(time, glfwGetTimerValue());
}

XudResult xud_time_mark(XudTime* time, uint64_t tick) {
    if (time == NULL) {
        return XUD_INVALID_ARG;
    }

    time->previousMarkTick = time->markTick;
    time->markTick = tick;
    time->boundaries += 1u;
    return XUD_OK;
}

XudResult xud_time_ticks_diff(uint64_t* outDiff, uint64_t later, uint64_t earlier) {
    if (outDiff == NULL) {
        return XUD_INVALID_ARG;
    }

    *outDiff = (later < earlier) ? 0u : (later - earlier);
    return XUD_OK;
}

XudResult xud_time_seconds_from_ticks(double* outSeconds, uint64_t tickCount, uint64_t frequency) {
    if (outSeconds == NULL) {
        return XUD_INVALID_ARG;
    }

    *outSeconds = 0.0;
    if (frequency == 0u) {
        return XUD_UNSUPPORTED;
    }

    *outSeconds = (double)tickCount / (double)frequency;
    return XUD_OK;
}

XudResult xud_time_elapsed(double* outSeconds, const XudTime* time) {
    uint64_t now = 0u;
    uint64_t span = 0u;
    XudResult result = XUD_INVALID_ARG;

    if (outSeconds == NULL) {
        return XUD_INVALID_ARG;
    }
    *outSeconds = 0.0;
    if (time == NULL) {
        return XUD_INVALID_ARG;
    }

    now = glfwGetTimerValue();
    result = xud_time_ticks_diff(&span, now, time->epochTick);
    if (result != XUD_OK) {
        return result;
    }

    return xud_time_seconds_from_ticks(outSeconds, span, glfwGetTimerFrequency());
}

XudResult xud_time_frame_time(double* outSeconds, const XudTime* time) {
    uint64_t span = 0u;
    XudResult result = XUD_INVALID_ARG;

    if (outSeconds == NULL) {
        return XUD_INVALID_ARG;
    }
    *outSeconds = 0.0;
    if (time == NULL) {
        return XUD_INVALID_ARG;
    }

    /* Below two boundaries there is no completed frame to answer about: one boundary is the frame now
       being drawn, and the frame below it has no tick to be measured against. 0.0 is that answer -
       the same reason the public query above this one states, and the reason a first frame's step i
       s zero rather than garbage.
    */
    if (time->boundaries < 2u) {
        return XUD_OK;
    }

    result = xud_time_ticks_diff(&span, time->markTick, time->previousMarkTick);
    if (result != XUD_OK) {
        return result;
    }

    return xud_time_seconds_from_ticks(outSeconds, span, glfwGetTimerFrequency());
}

XudResult xud_time_close(XudTime** time) {
    if (time == NULL) {
        return XUD_INVALID_ARG;
    }
    if (*time == NULL) {
        return XUD_ALREADY;
    }

    free(*time);
    *time = NULL;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The public clock calls, above the rung
 *
 * One guard shape for all four: the rung is a bring-up requirement, so a context that has no clock
 * rung is a context that never completed bring-up and answers XUD_UNSUPPORTED rather than reading a
 * tick nothing owns. Nothing here polls GLFW on demand beyond the one read a query needs - the elap
 * sed is live by design (raylib's GetTime is) and the frame span is the rung's own record, so the
 * only thing a query can disagree with is the clock itself.
 *-----------------------------------------------------------------------------------------------*/

static XudResult xud_time_reach(XudTime** outTime, XudContext* context) {
    if (context == NULL) {
        return XUD_INVALID_ARG;
    }
    if (context->time == NULL) {
        return XUD_UNSUPPORTED;
    }

    *outTime = context->time;
    return XUD_OK;
}

/// See xud.h.
XudResult xud_get_time(double* outSeconds, XudContext* context) {
    XudTime* time = NULL;
    XudResult result = XUD_INVALID_ARG;

    if (outSeconds == NULL) {
        return XUD_INVALID_ARG;
    }
    *outSeconds = 0.0;

    result = xud_time_reach(&time, context);
    if (result != XUD_OK) {
        return result;
    }

    return xud_time_elapsed(outSeconds, time);
}

/// See xud.h.
XudResult xud_get_frame_time(float* outSeconds, XudContext* context) {
    XudTime* time = NULL;
    double seconds = 0.0;
    XudResult result = XUD_INVALID_ARG;

    if (outSeconds == NULL) {
        return XUD_INVALID_ARG;
    }
    *outSeconds = 0.0f;

    result = xud_time_reach(&time, context);
    if (result != XUD_OK) {
        return result;
    }

    result = xud_time_frame_time(&seconds, time);
    if (result != XUD_OK) {
        return result;
    }

    *outSeconds = (float)seconds;
    return XUD_OK;
}

/// See xud.h.
XudResult xud_get_timer_value(uint64_t* outTicks, XudContext* context) {
    XudTime* time = NULL;
    XudResult result = XUD_INVALID_ARG;

    if (outTicks == NULL) {
        return XUD_INVALID_ARG;
    }
    *outTicks = 0u;

    result = xud_time_reach(&time, context);
    if (result != XUD_OK) {
        return result;
    }

    *outTicks = glfwGetTimerValue();
    return XUD_OK;
}

/// See xud.h.
XudResult xud_get_timer_frequency(uint64_t* outTicksPerSecond, XudContext* context) {
    XudTime* time = NULL;
    XudResult result = XUD_INVALID_ARG;

    if (outTicksPerSecond == NULL) {
        return XUD_INVALID_ARG;
    }
    *outTicksPerSecond = 0u;

    result = xud_time_reach(&time, context);
    if (result != XUD_OK) {
        return result;
    }

    *outTicksPerSecond = glfwGetTimerFrequency();
    return XUD_OK;
}
