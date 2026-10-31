/***********************************************************************************************
 *
 *   xud_input.c - the input rung: one GLFW window's key and mouse state, kept as a state machine
 *                 the public input API above it reads
 *
 *   Owns : the four rungs xud_input.h declares (open, poll, the pure advance, close) and the public
 *          input calls xud.h declares above them, which are those records read through the one con
 *          text guard. The split is the point of the file: xud_input_poll() is the only call here
 *          that reaches GLFW's input state, and everything the answers MEAN - the transitions, the
 *          queues, the cursor delta, the wheel - is xud_input_advance(), which takes a snapshot of
 *          bytes rather than a window and can therefore be checked with no window system and no dr
 *          iver present. The public calls are the same records read through a context, so they are
 *          headless too, and that is why the default test run asserts the whole of this rung.
 *
 *   What the rung owns and what it borrows: it owns one host allocation, two callbacks registered
 *   on the window, the window's user pointer (GLFW's own payload slot, and the only way its char a
 *   nd scroll callbacks can find the rung they belong to) and the state rows. It borrows the windo
 *   w: it never destroys it, and xud_input_close() runs while the GLFW window is still alive for
 *   exactly that reason - a callback naming a freed rung is not something teardown leaves behind.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include "xud_input.h" /* Required for: XudInput, the rung's constants and the four declarations this file answers */
#include "xud.h" /* Required for: XudResult and the public tokens the queries range-check */
#include "xud_core.h" /* Required for: struct XudContext, whose input field the public calls reach */
#include "xud_window.h" /* Required for: XudWindow, the window whose GLFWwindow* the poll reads */
#include <GLFW/glfw3.h> /* Required for: glfwPollEvents, glfwGetKey, glfwGetMouseButton, glfwGetCursorPos, glfwSetCursorPos, glfwGetKeyName, glfwGet/SetWindowUserPointer, glfwSetChar/ScrollCallback, glfwGetError - the whole of GLFW's input surface, and the only place in src/xud besides the window layer that calls GLFW at all (examples/breakout_3d.c calls GLFW's timer, which is the clock rung and not this one) */
#include <math.h> /* Required for: isfinite, the scale the setter refuses */
#include <stdlib.h> /* Required for: calloc, free */

/*-----------------------------------------------------------------------------------------------
 * The two callbacks GLFW's window carries
 *
 * A codepoint and a scroll delta are the only two pieces of input GLFW's Wayland backend delivers
 * to anything other than a state query: wl_window.c 1372 calls _glfwInputChar, which calls the wi
 * ndow's character callback, and wl_window.c 1605 calls _glfwInputScroll, which calls the window's
 * scroll callback. There is no glfwGetChar and no glfwGetScroll to read instead - grepped in the
 * vendored 3.5.1 headers, neither name exists - so the rung registers both and the queues and rows
 * below are what those two calls fill.
 *
 * The callback's only argument is the GLFWwindow*, so the rung finds itself through GLFW's own pay
 * load slot for that window rather than through a static this file could not keep (AGENTS.md: no g
 * lobal mutable state except the one library context). The slot is the window layer's to own and n
 * othing else in this tree takes it - xud_input_open() checks that before it fills it, and xud_inpu
 * t_close() clears it.
 *-----------------------------------------------------------------------------------------------*/

static void xud_input_char_event(GLFWwindow* glfwWindow, unsigned int codepoint) {
    XudInput* input = NULL;

    if (glfwWindow == NULL) {
        return;
    }
    input = (XudInput*)glfwGetWindowUserPointer(glfwWindow);
    if (input == NULL) {
        return;
    }

    if (input->charQueueCount < XUD_INPUT_CHAR_QUEUE_MAX) {
        input->charQueue[input->charQueueCount] = (int32_t)codepoint;
        input->charQueueCount += 1u;
    } else {
        /* More codepoints arrived between two polls than the queue holds. The overflow cannot be
           reported from here - a callback answers nothing - so it is recorded and the query that
           drains the queue reports it once, which is what makes a cut list a report rather than a
           silent truncation.
        */
        input->charTruncated = 1u;
    }
}

static void xud_input_scroll_event(GLFWwindow* glfwWindow, double xoffset, double yoffset) {
    XudInput* input = NULL;

    if (glfwWindow == NULL) {
        return;
    }
    input = (XudInput*)glfwGetWindowUserPointer(glfwWindow);
    if (input == NULL) {
        return;
    }

    /* Accumulated rather than stored: a frame can be scrolled several times before the poll above
       it runs, and raylib's GetMouseWheelMove answers the frame's whole movement, not its last on
       e.
    */
    input->wheelPendingX += xoffset;
    input->wheelPendingY += yoffset;
}

/*-----------------------------------------------------------------------------------------------
 * Bring-up and teardown
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_input_open(XudInput** outInput, XudWindow* window) {
    XudInput* input = NULL;
    GLFWcharfun previousChar = NULL;
    GLFWscrollfun previousScroll = NULL;

    if ((outInput == NULL) || (*outInput != NULL) || (window == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (window->glfwWindow == NULL) {
        return XUD_UNSUPPORTED;
    }

    /* GLFW's payload slot has to be free before this rung fills it: the two callbacks below find th
       is rung through that slot, so a slot already taken is a window someone else is already read
       ing input above - and two rungs reading one window's state through one slot is not a state t
       his tree can be in.
    */
    if (glfwGetWindowUserPointer(window->glfwWindow) != NULL) {
        return XUD_INTERNAL_ERROR;
    }

    input = calloc(1, sizeof(XudInput));
    if (input == NULL) {
        return XUD_OUT_OF_MEMORY;
    }

    /* The zero state calloc gives is the rung's opening state for every row EXCEPT these two: rayli
       b's SetExitKey says ESC is the default (raylib.h 1210), and a scale of zero would be a divi
       sion the position queries do behind the setter's back, so the opening scale is the identity.
    */
    input->window = window;
    input->exitKey = XUD_KEY_ESCAPE;
    input->scaleX = 1.0;
    input->scaleY = 1.0;

    glfwSetWindowUserPointer(window->glfwWindow, input);

    /* GLFW's setters SWAP the callback in and hand the previous one back (window.c: _GLFW_SWAP), so
       a non-NULL answer is a callback someone else owned. Putting theirs back and refusing is what
       keeps this rung from being the thing that stole it.
    */
    previousChar = glfwSetCharCallback(window->glfwWindow, xud_input_char_event);
    if (previousChar != NULL) {
        (void)glfwSetCharCallback(window->glfwWindow, previousChar);
        glfwSetWindowUserPointer(window->glfwWindow, NULL);
        free(input);
        return XUD_INTERNAL_ERROR;
    }
    previousScroll = glfwSetScrollCallback(window->glfwWindow, xud_input_scroll_event);
    if (previousScroll != NULL) {
        (void)glfwSetScrollCallback(window->glfwWindow, previousScroll);
        (void)glfwSetCharCallback(window->glfwWindow, NULL);
        glfwSetWindowUserPointer(window->glfwWindow, NULL);
        free(input);
        return XUD_INTERNAL_ERROR;
    }

    *outInput = input;
    return XUD_OK;
}

XudResult xud_input_close(XudInput** input) {
    XudInput* victim = NULL;

    if (input == NULL) {
        return XUD_INVALID_ARG;
    }
    if (*input == NULL) {
        return XUD_ALREADY;
    }

    victim = *input;

    /* Unregistered while the GLFW window is still alive - the window layer's own close is what dest
       roys that window, and this rung goes before it in the teardown order for exactly this reason:
       a char or scroll event arriving after this point would call a callback naming freed memory.
       Clearing the payload slot is the other half of the same promise: the window outlives this ru
       ng, so a slot naming it must not.
    */
    if ((victim->window != NULL) && (victim->window->glfwWindow != NULL)) {
        (void)glfwSetCharCallback(victim->window->glfwWindow, NULL);
        (void)glfwSetScrollCallback(victim->window->glfwWindow, NULL);
        glfwSetWindowUserPointer(victim->window->glfwWindow, NULL);
    }

    free(victim);
    *input = NULL;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The pure half - the state machine, above a snapshot of bytes
 *
 * The snapshot is an array rather than a window for the reason the header states: the transition ta
 * ble, the queues and the cursor delta below are the whole of this rung's correctness, and none of
 * them needs a window system to be asserted. The snapshot is validated whole before a single row i
 * s written, which is what makes a refused advance change nothing rather than half a frame's state.
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_input_advance(XudInput* input, const uint8_t keyStates[XUD_INPUT_KEY_COUNT],
    const uint8_t buttonStates[XUD_INPUT_BUTTON_COUNT], double cursorRawX, double cursorRawY) {
    uint32_t i = 0u;

    if ((input == NULL) || (keyStates == NULL) || (buttonStates == NULL)) {
        return XUD_INVALID_ARG;
    }

    /* Every byte of the snapshot before any of the rows: GLFW answers three states for a key and th
       ree for a button (the assert in the header above pins the numbers), and a byte outside them i
       s a state this rung cannot name - GLFW's sticky mode is the fourth value that would come he
       re, and nothing in this tree asks for it (no glfwSetStickyKeysInputMode, no glfwSetStickyMouse
       ButtonsInputMode), so refusing is reporting a state that cannot happen rather than reading a
       row this layer would misname.
    */
    for (i = 0u; i < XUD_INPUT_KEY_COUNT; i += 1u) {
        if ((keyStates[i] != XUD_INPUT_STATE_UP) && (keyStates[i] != XUD_INPUT_STATE_DOWN) && (keyStates[i] != XUD_INPUT_STATE_REPEAT)) {
            return XUD_INVALID_ARG;
        }
    }
    for (i = 0u; i < XUD_INPUT_BUTTON_COUNT; i += 1u) {
        if ((buttonStates[i] != XUD_INPUT_STATE_UP) && (buttonStates[i] != XUD_INPUT_STATE_DOWN)
            && (buttonStates[i] != XUD_INPUT_STATE_REPEAT)) {
            return XUD_INVALID_ARG;
        }
    }

    /* The transition: the current row moves into the previous slot and the snapshot becomes the cur
       rent one, so a query reads a pair that belongs to one boundary between two polls rather than
       to two different moments. A key only transitions into "down" once between two polls, which is
       the proof beside the queue's bound in the header: the push below cannot overflow.
    */
    for (i = 0u; i < XUD_INPUT_KEY_COUNT; i += 1u) {
        input->keyPrevious[i] = input->keyState[i];
        input->keyState[i] = keyStates[i];
        if ((keyStates[i] != XUD_INPUT_STATE_UP) && (input->keyPrevious[i] == XUD_INPUT_STATE_UP)) {
            input->keyQueue[input->keyQueueCount] = (int32_t)(i + XUD_INPUT_KEY_FIRST);
            input->keyQueueCount += 1u;
        }
    }
    for (i = 0u; i < XUD_INPUT_BUTTON_COUNT; i += 1u) {
        input->buttonPrevious[i] = input->buttonState[i];
        input->buttonState[i] = buttonStates[i];
    }

    /* The cursor: the delta is this poll's raw position against the poll below it, which is the "be
       tween frames" of raylib's GetMouseDelta stated against the two polls rather than against the
       two calls, because nothing here polls on demand. The transform above it is raylib's own - off
       set first, then scale - and the setter refused a zero and a non-finite scale, so the division
       below reads neither.
    */
    input->deltaCursorX = cursorRawX - input->cursorRawX;
    input->deltaCursorY = cursorRawY - input->cursorRawY;
    input->cursorRawX = cursorRawX;
    input->cursorRawY = cursorRawY;
    input->cursorX = (cursorRawX - input->offsetX) / input->scaleX;
    input->cursorY = (cursorRawY - input->offsetY) / input->scaleY;

    /* The wheel: what the callbacks accumulated since the poll below this one becomes what a caller
       reads, and the accumulator is started again for the frame above it.
    */
    input->wheelX = input->wheelPendingX;
    input->wheelY = input->wheelPendingY;
    input->wheelPendingX = 0.0;
    input->wheelPendingY = 0.0;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The poll - the thin half above GLFW
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_input_poll(XudInput* input) {
    uint8_t keyStates[XUD_INPUT_KEY_COUNT];
    uint8_t buttonStates[XUD_INPUT_BUTTON_COUNT];
    double cursorRawX = 0.0;
    double cursorRawY = 0.0;
    uint32_t i = 0u;

    if (input == NULL) {
        return XUD_INVALID_ARG;
    }
    if ((input->window == NULL) || (input->window->glfwWindow == NULL)) {
        return XUD_UNSUPPORTED;
    }

    /* One event loop per frame, and it is the only call in this tree that runs one: GLFW's Wayland
       backend delivers a key event only when its loop runs (wl_window.c 1450 is where a key event i
       s read out of the compositor's queue), so a rung that never polled would answer "up" for eve
       ry key forever. raylib's own frame pair puts the poll at the end of a frame too (raylib.h 110
       0), which is why this call sits in xud_end_drawing() rather than in a query.
    */
    glfwPollEvents();

    /* The whole range GLFW's key state covers, read as one snapshot. glfwGetKey refuses a token out
       side exactly this range (input.c 749-756), so the scan is the range and not a wider one with
       a check inside it, and the cost is a scan of 317 array reads per frame - the state is GLF
       W's per-key query and this tree has no other way to reach it, since GLFW's window struct is
       opaque behind the window layer's own pointer.
    */
    for (i = 0u; i < XUD_INPUT_KEY_COUNT; i += 1u) {
        keyStates[i] = (uint8_t)glfwGetKey(input->window->glfwWindow, (int)(i + XUD_INPUT_KEY_FIRST));
    }
    for (i = 0u; i < XUD_INPUT_BUTTON_COUNT; i += 1u) {
        buttonStates[i] = (uint8_t)glfwGetMouseButton(input->window->glfwWindow, (int)(i + XUD_INPUT_BUTTON_FIRST));
    }
    glfwGetCursorPos(input->window->glfwWindow, &cursorRawX, &cursorRawY);

    return xud_input_advance(input, keyStates, buttonStates, cursorRawX, cursorRawY);
}

/*-----------------------------------------------------------------------------------------------
 * The public input calls above the rung
 *
 * Every one of them is the same three lines: the context guard, the argument range, the record. The
 * guard is a helper rather than a copy per call because there are twenty-three of them and a guard
 * that drifts between two of them is a query that answers about the wrong window - and the range is
 * GLFW's rather than a wider one this rung would have to clamp, because the state rows are indexed
 * by GLFW's tokens (the mirror assert in the header above is what makes that indexing the same num
 * bering the public tokens name).
 *-----------------------------------------------------------------------------------------------*/

static XudResult xud_input_reach(XudInput** outInput, XudContext* context) {
    if (context == NULL) {
        return XUD_INVALID_ARG;
    }
    if (context->input == NULL) {
        return XUD_UNSUPPORTED;
    }
    *outInput = context->input;
    return XUD_OK;
}

/// See xud.h.
XudResult xud_is_key_pressed(bool* outPressed, XudContext* context, int key) {
    XudInput* input = NULL;
    XudResult result = XUD_INVALID_ARG;

    if (outPressed == NULL) {
        return XUD_INVALID_ARG;
    }
    *outPressed = false;

    result = xud_input_reach(&input, context);
    if (result != XUD_OK) {
        return result;
    }
    if ((key < XUD_INPUT_KEY_FIRST) || (key > XUD_INPUT_KEY_LAST)) {
        return XUD_INVALID_ARG;
    }

    *outPressed = (input->keyState[key - XUD_INPUT_KEY_FIRST] != XUD_INPUT_STATE_UP)
        && (input->keyPrevious[key - XUD_INPUT_KEY_FIRST] == XUD_INPUT_STATE_UP);
    return XUD_OK;
}

/// See xud.h.
XudResult xud_is_key_pressed_repeat(bool* outRepeat, XudContext* context, int key) {
    XudInput* input = NULL;
    XudResult result = XUD_INVALID_ARG;

    if (outRepeat == NULL) {
        return XUD_INVALID_ARG;
    }
    *outRepeat = false;

    result = xud_input_reach(&input, context);
    if (result != XUD_OK) {
        return result;
    }
    if ((key < XUD_INPUT_KEY_FIRST) || (key > XUD_INPUT_KEY_LAST)) {
        return XUD_INVALID_ARG;
    }

    *outRepeat = (input->keyState[key - XUD_INPUT_KEY_FIRST] == XUD_INPUT_STATE_REPEAT);
    return XUD_OK;
}

/// See xud.h.
XudResult xud_is_key_down(bool* outDown, XudContext* context, int key) {
    XudInput* input = NULL;
    XudResult result = XUD_INVALID_ARG;

    if (outDown == NULL) {
        return XUD_INVALID_ARG;
    }
    *outDown = false;

    result = xud_input_reach(&input, context);
    if (result != XUD_OK) {
        return result;
    }
    if ((key < XUD_INPUT_KEY_FIRST) || (key > XUD_INPUT_KEY_LAST)) {
        return XUD_INVALID_ARG;
    }

    *outDown = (input->keyState[key - XUD_INPUT_KEY_FIRST] != XUD_INPUT_STATE_UP);
    return XUD_OK;
}

/// See xud.h.
XudResult xud_is_key_released(bool* outReleased, XudContext* context, int key) {
    XudInput* input = NULL;
    XudResult result = XUD_INVALID_ARG;

    if (outReleased == NULL) {
        return XUD_INVALID_ARG;
    }
    *outReleased = false;

    result = xud_input_reach(&input, context);
    if (result != XUD_OK) {
        return result;
    }
    if ((key < XUD_INPUT_KEY_FIRST) || (key > XUD_INPUT_KEY_LAST)) {
        return XUD_INVALID_ARG;
    }

    *outReleased = (input->keyState[key - XUD_INPUT_KEY_FIRST] == XUD_INPUT_STATE_UP)
        && (input->keyPrevious[key - XUD_INPUT_KEY_FIRST] != XUD_INPUT_STATE_UP);
    return XUD_OK;
}

/// See xud.h.
XudResult xud_is_key_up(bool* outUp, XudContext* context, int key) {
    XudInput* input = NULL;
    XudResult result = XUD_INVALID_ARG;

    if (outUp == NULL) {
        return XUD_INVALID_ARG;
    }
    *outUp = false;

    result = xud_input_reach(&input, context);
    if (result != XUD_OK) {
        return result;
    }
    if ((key < XUD_INPUT_KEY_FIRST) || (key > XUD_INPUT_KEY_LAST)) {
        return XUD_INVALID_ARG;
    }

    *outUp = (input->keyState[key - XUD_INPUT_KEY_FIRST] == XUD_INPUT_STATE_UP);
    return XUD_OK;
}

/// See xud.h. The queue is drained from the front, so the order a caller reads the keys in is the or
/// der the poll's scan found them - the scan is by token index, which is why two keys pressed betw
/// een the same two polls come back in token order rather than in the order the window saw them.
XudResult xud_get_key_pressed(int* outKey, XudContext* context) {
    XudInput* input = NULL;
    XudResult result = XUD_INVALID_ARG;
    uint32_t i = 0u;

    if (outKey == NULL) {
        return XUD_INVALID_ARG;
    }
    *outKey = 0;

    result = xud_input_reach(&input, context);
    if (result != XUD_OK) {
        return result;
    }

    /* The drain reads the front BEFORE the queue is compacted - compacting first would hand back th
       e second key rather than the first, which is the difference between "the order the scan found
       them" and "the order with the oldest key dropped".
    */
    if (input->keyQueueCount == 0u) {
        return XUD_OK;
    }

    *outKey = (int)input->keyQueue[0];
    input->keyQueueCount -= 1u;
    for (i = 1u; i <= input->keyQueueCount; i += 1u) {
        input->keyQueue[i - 1u] = input->keyQueue[i];
    }
    return XUD_OK;
}

/// See xud.h. Same drain, and the truncation is reported once the queue is empty so a caller sees th
/// e whole list it got and the fact that it was cut, in that order.
XudResult xud_get_char_pressed(int* outChar, XudContext* context) {
    XudInput* input = NULL;
    XudResult result = XUD_INVALID_ARG;
    uint32_t i = 0u;

    if (outChar == NULL) {
        return XUD_INVALID_ARG;
    }
    *outChar = 0;

    result = xud_input_reach(&input, context);
    if (result != XUD_OK) {
        return result;
    }

    if (input->charQueueCount > 0u) {
        *outChar = (int)input->charQueue[0];
        input->charQueueCount -= 1u;
        for (i = 1u; i <= input->charQueueCount; i += 1u) {
            input->charQueue[i - 1u] = input->charQueue[i];
        }
        return XUD_OK;
    }
    if (input->charTruncated != 0u) {
        input->charTruncated = 0u;
        return XUD_OUT_OF_RESOURCES;
    }
    return XUD_OK;
}

/// See xud.h. The name is GLFW's own: glfwGetKeyName resolves the key on the CURRENT layout out of
/// the window system's keyboard state (the Wayland half, wl_window.c 1822, is where the compositor's
/// keymap arrives, and wl_window.c 2981 is where the name is read out of it), so the pointer is a bo
/// rowed view rather than storage this rung owns, and a layout change rewrites it. The window guard
/// below is what makes that true rather than a hope: the keymap is delivered to a window that asked
/// for focus, so with no window there is no layout to resolve a name against.
XudResult xud_get_key_name(const char** outName, XudContext* context, int key) {
    XudInput* input = NULL;
    XudResult result = XUD_INVALID_ARG;

    if (outName == NULL) {
        return XUD_INVALID_ARG;
    }
    *outName = NULL;

    result = xud_input_reach(&input, context);
    if (result != XUD_OK) {
        return result;
    }
    if ((input->window == NULL) || (input->window->glfwWindow == NULL)) {
        return XUD_UNSUPPORTED;
    }
    if ((key < XUD_INPUT_KEY_FIRST) || (key > XUD_INPUT_KEY_LAST)) {
        return XUD_INVALID_ARG;
    }

    /* glfwGetKeyName takes a scancode as well as a token - the pair GLFW uses when it answers GLF
       W_KEY_UNKNOWN for a key it cannot name - and this rung queries by token, so the scancode it
       gives is 0 and the token decides. It refuses a token outside exactly the range checked above
       (input.c 710), and it answers NULL for a token that types nothing (input.c 716-719), which i
       s why NULL is the answer rather than a refusal.
    */
    *outName = glfwGetKeyName(key, 0);
    return XUD_OK;
}

/// See xud.h.
XudResult xud_set_exit_key(XudContext* context, int key) {
    XudInput* input = NULL;
    XudResult result = XUD_INVALID_ARG;

    result = xud_input_reach(&input, context);
    if (result != XUD_OK) {
        return result;
    }
    if ((key != XUD_KEY_NULL) && ((key < XUD_INPUT_KEY_FIRST) || (key > XUD_INPUT_KEY_LAST))) {
        return XUD_INVALID_ARG;
    }

    input->exitKey = key;
    return XUD_OK;
}

/// See xud.h.
XudResult xud_get_exit_key(int* outKey, XudContext* context) {
    XudInput* input = NULL;
    XudResult result = XUD_INVALID_ARG;

    if (outKey == NULL) {
        return XUD_INVALID_ARG;
    }
    *outKey = 0;

    result = xud_input_reach(&input, context);
    if (result != XUD_OK) {
        return result;
    }

    *outKey = (int)input->exitKey;
    return XUD_OK;
}

/// See xud.h.
XudResult xud_is_mouse_button_pressed(bool* outPressed, XudContext* context, int button) {
    XudInput* input = NULL;
    XudResult result = XUD_INVALID_ARG;

    if (outPressed == NULL) {
        return XUD_INVALID_ARG;
    }
    *outPressed = false;

    result = xud_input_reach(&input, context);
    if (result != XUD_OK) {
        return result;
    }
    if ((button < XUD_INPUT_BUTTON_FIRST) || (button > XUD_INPUT_BUTTON_LAST)) {
        return XUD_INVALID_ARG;
    }

    *outPressed = (input->buttonState[button - XUD_INPUT_BUTTON_FIRST] != XUD_INPUT_STATE_UP)
        && (input->buttonPrevious[button - XUD_INPUT_BUTTON_FIRST] == XUD_INPUT_STATE_UP);
    return XUD_OK;
}

/// See xud.h.
XudResult xud_is_mouse_button_down(bool* outDown, XudContext* context, int button) {
    XudInput* input = NULL;
    XudResult result = XUD_INVALID_ARG;

    if (outDown == NULL) {
        return XUD_INVALID_ARG;
    }
    *outDown = false;

    result = xud_input_reach(&input, context);
    if (result != XUD_OK) {
        return result;
    }
    if ((button < XUD_INPUT_BUTTON_FIRST) || (button > XUD_INPUT_BUTTON_LAST)) {
        return XUD_INVALID_ARG;
    }

    *outDown = (input->buttonState[button - XUD_INPUT_BUTTON_FIRST] != XUD_INPUT_STATE_UP);
    return XUD_OK;
}

/// See xud.h.
XudResult xud_is_mouse_button_released(bool* outReleased, XudContext* context, int button) {
    XudInput* input = NULL;
    XudResult result = XUD_INVALID_ARG;

    if (outReleased == NULL) {
        return XUD_INVALID_ARG;
    }
    *outReleased = false;

    result = xud_input_reach(&input, context);
    if (result != XUD_OK) {
        return result;
    }
    if ((button < XUD_INPUT_BUTTON_FIRST) || (button > XUD_INPUT_BUTTON_LAST)) {
        return XUD_INVALID_ARG;
    }

    *outReleased = (input->buttonState[button - XUD_INPUT_BUTTON_FIRST] == XUD_INPUT_STATE_UP)
        && (input->buttonPrevious[button - XUD_INPUT_BUTTON_FIRST] != XUD_INPUT_STATE_UP);
    return XUD_OK;
}

/// See xud.h.
XudResult xud_is_mouse_button_up(bool* outUp, XudContext* context, int button) {
    XudInput* input = NULL;
    XudResult result = XUD_INVALID_ARG;

    if (outUp == NULL) {
        return XUD_INVALID_ARG;
    }
    *outUp = false;

    result = xud_input_reach(&input, context);
    if (result != XUD_OK) {
        return result;
    }
    if ((button < XUD_INPUT_BUTTON_FIRST) || (button > XUD_INPUT_BUTTON_LAST)) {
        return XUD_INVALID_ARG;
    }

    *outUp = (input->buttonState[button - XUD_INPUT_BUTTON_FIRST] == XUD_INPUT_STATE_UP);
    return XUD_OK;
}

/// See xud.h. The int is raylib's own return type for this query (raylib.h 1232/1233): the rung's cu
/// rsor is a double because the offset and scale above it are not integers, and the cast is the con
/// version back to the pixel grid a caller places a shape on.
XudResult xud_get_mouse_x(int* outX, XudContext* context) {
    XudInput* input = NULL;
    XudResult result = XUD_INVALID_ARG;

    if (outX == NULL) {
        return XUD_INVALID_ARG;
    }
    *outX = 0;

    result = xud_input_reach(&input, context);
    if (result != XUD_OK) {
        return result;
    }

    *outX = (int)input->cursorX;
    return XUD_OK;
}

/// See xud.h.
XudResult xud_get_mouse_y(int* outY, XudContext* context) {
    XudInput* input = NULL;
    XudResult result = XUD_INVALID_ARG;

    if (outY == NULL) {
        return XUD_INVALID_ARG;
    }
    *outY = 0;

    result = xud_input_reach(&input, context);
    if (result != XUD_OK) {
        return result;
    }

    *outY = (int)input->cursorY;
    return XUD_OK;
}

/// See xud.h.
XudResult xud_get_mouse_position(XudVector2* outPosition, XudContext* context) {
    XudInput* input = NULL;
    XudResult result = XUD_INVALID_ARG;

    if (outPosition == NULL) {
        return XUD_INVALID_ARG;
    }
    outPosition->x = 0.0f;
    outPosition->y = 0.0f;

    result = xud_input_reach(&input, context);
    if (result != XUD_OK) {
        return result;
    }

    outPosition->x = (float)input->cursorX;
    outPosition->y = (float)input->cursorY;
    return XUD_OK;
}

/// See xud.h.
XudResult xud_get_mouse_delta(XudVector2* outDelta, XudContext* context) {
    XudInput* input = NULL;
    XudResult result = XUD_INVALID_ARG;

    if (outDelta == NULL) {
        return XUD_INVALID_ARG;
    }
    outDelta->x = 0.0f;
    outDelta->y = 0.0f;

    result = xud_input_reach(&input, context);
    if (result != XUD_OK) {
        return result;
    }

    outDelta->x = (float)input->deltaCursorX;
    outDelta->y = (float)input->deltaCursorY;
    return XUD_OK;
}

/// See xud.h. The refusal is read back out of GLFW rather than assumed: glfwSetCursorPos returns sil
/// ently when the window has no keyboard focus (input.c 826) and its Wayland half answers an error
/// without moving anything (wl_window.c 2970), so the two signals GLFW gives - the error buffer and
/// the position that did not go where it was asked - are both read, and either one is a refusal. Th
/// e error buffer is drained before the call so the answer read after it is this call's and not a
/// leftover from somewhere else in the frame.
XudResult xud_set_mouse_position(XudContext* context, int x, int y) {
    XudInput* input = NULL;
    XudResult result = XUD_INVALID_ARG;
    const char* error = NULL;
    double placedX = 0.0;
    double placedY = 0.0;

    result = xud_input_reach(&input, context);
    if (result != XUD_OK) {
        return result;
    }
    if ((input->window == NULL) || (input->window->glfwWindow == NULL)) {
        return XUD_UNSUPPORTED;
    }

    (void)glfwGetError(&error);

    glfwSetCursorPos(input->window->glfwWindow, (double)x, (double)y);

    if (glfwGetError(&error) != GLFW_NO_ERROR) {
        return XUD_UNSUPPORTED;
    }

    glfwGetCursorPos(input->window->glfwWindow, &placedX, &placedY);
    if ((placedX != (double)x) || (placedY != (double)y)) {
        return XUD_UNSUPPORTED;
    }

    /* The position the caller asked for is now where the cursor is, so the rung's own view of it is
       the transform above that position - the same pair the position queries read, so a caller that
       placed the cursor and then asked where it is gets the number it asked for through one transfo
       rm rather than two different ones.
    */
    input->cursorRawX = (double)x;
    input->cursorRawY = (double)y;
    input->cursorX = (input->cursorRawX - input->offsetX) / input->scaleX;
    input->cursorY = (input->cursorRawY - input->offsetY) / input->scaleY;
    return XUD_OK;
}

/// See xud.h.
XudResult xud_set_mouse_offset(XudContext* context, int offsetX, int offsetY) {
    XudInput* input = NULL;
    XudResult result = XUD_INVALID_ARG;

    result = xud_input_reach(&input, context);
    if (result != XUD_OK) {
        return result;
    }

    input->offsetX = (double)offsetX;
    input->offsetY = (double)offsetY;

    /* The cached cursor is re-derived through the transform the position queries read, so a caller t
       hat moved the origin mid-frame reads the position it asked for at once rather than at the nex
       t poll - one transform, one answer, and no stale half of it.
    */
    input->cursorX = (input->cursorRawX - input->offsetX) / input->scaleX;
    input->cursorY = (input->cursorRawY - input->offsetY) / input->scaleY;
    return XUD_OK;
}

/// See xud.h. A scale of zero is refused rather than stored: the position queries divide by it, and a
/// rung that stored one would answer an infinity behind every caller's back rather than a refusal.
XudResult xud_set_mouse_scale(XudContext* context, float scaleX, float scaleY) {
    XudInput* input = NULL;
    XudResult result = XUD_INVALID_ARG;

    result = xud_input_reach(&input, context);
    if (result != XUD_OK) {
        return result;
    }
    if (!isfinite((double)scaleX) || !isfinite((double)scaleY) || (scaleX == 0.0f) || (scaleY == 0.0f)) {
        return XUD_INVALID_ARG;
    }

    input->scaleX = (double)scaleX;
    input->scaleY = (double)scaleY;

    /* The cached cursor is re-derived the same way the offset setter does it, above the same pair th
       e position queries read.
    */
    input->cursorX = (input->cursorRawX - input->offsetX) / input->scaleX;
    input->cursorY = (input->cursorRawY - input->offsetY) / input->scaleY;
    return XUD_OK;
}

/// See xud.h. raylib's GetMouseWheelMove is "X or Y, whichever is larger" (raylib.h 1239) - larger by
/// absolute value, with the sign kept, because a scroll that went down is a negative movement rathe
/// r than a smaller one.
XudResult xud_get_mouse_wheel_move(float* outMove, XudContext* context) {
    XudInput* input = NULL;
    XudResult result = XUD_INVALID_ARG;

    if (outMove == NULL) {
        return XUD_INVALID_ARG;
    }
    *outMove = 0.0f;

    result = xud_input_reach(&input, context);
    if (result != XUD_OK) {
        return result;
    }

    *outMove = ((input->wheelX >= 0.0) ? input->wheelX : -input->wheelX) >= ((input->wheelY >= 0.0) ? input->wheelY : -input->wheelY)
        ? (float)input->wheelX
        : (float)input->wheelY;
    return XUD_OK;
}

/// See xud.h.
XudResult xud_get_mouse_wheel_move_v(XudVector2* outMove, XudContext* context) {
    XudInput* input = NULL;
    XudResult result = XUD_INVALID_ARG;

    if (outMove == NULL) {
        return XUD_INVALID_ARG;
    }
    outMove->x = 0.0f;
    outMove->y = 0.0f;

    result = xud_input_reach(&input, context);
    if (result != XUD_OK) {
        return result;
    }

    outMove->x = (float)input->wheelX;
    outMove->y = (float)input->wheelY;
    return XUD_OK;
}
