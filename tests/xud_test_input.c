/***********************************************************************************************
 *
 *   xud_test_input.c - the input rung: GLFW's key and mouse state kept as a state machine, and the
 *   public input API above it
 *
 *   Owns : the transition table (pressed / pressed-repeat / down / released / up for a key and for
 * a button), the two queues and their drains, the cursor's transform and delta, the wheel's accumu
 * lator, the exit-key record, the scale and offset setters' refusals, the key-name and cursor-place
 * guards, the rung's own open/close guards - plus the GPU half that polls a real window this con
 * text opened and reads the same records back. Layer: tests. Sees xud_input.h (the rung) and xud.h
 * (the public calls and the tokens), and xud_window.h for the GLFW payload slot the GPU half checks.
 *
 *   The pure half is the whole point of this area: xud_input_advance() takes a snapshot of bytes
 *   rather than a window, so the default run asserts the state machine with no window system and no
 *   driver present, and the public queries above it are the same records read through a context, so
 *   they are asserted the same way.
 *
 *   Deterministic: fixed frame counts, fixed snapshots, no time, no randomness, no filesystem.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud.h" /* Required for: XudResult, the public tokens, the public input calls, XudVector2 */
#include "xud_core.h" /* Required for: struct XudContext, whose input field the public calls reach */
#include "xud_input.h" /* Required for: XudInput, the rung's constants, xud_input_open/poll/advance/close */
#include "xud_test_harness.h" /* Required for: xud_check, xud_gpu_tests_enabled, the area entry points */
#include "xud_window.h" /* Required for: XudWindow (and, through it, <GLFW/glfw3.h> for the payload-slot check below) */
#include <math.h> /* Required for: NAN, the scale the setter must refuse */
#include <stdlib.h> /* Required for: calloc and free, the synthetic rung and shell this area builds */
#include <string.h> /* Required for: memset, the snapshots and the synthetic rows */

/* One frame's snapshot above xud_input_advance(): the whole range at UP, one key at the state asked
   for, the whole button range at UP. Every transition check below is a sequence of these, which is
   what makes the table read as frames rather than as hand-built arrays.
*/
static void xud_test_input_snapshot(
    uint8_t keyStates[XUD_INPUT_KEY_COUNT], uint8_t buttonStates[XUD_INPUT_BUTTON_COUNT], int key, uint8_t state) {
    memset(keyStates, XUD_INPUT_STATE_UP, XUD_INPUT_KEY_COUNT);
    memset(buttonStates, XUD_INPUT_STATE_UP, XUD_INPUT_BUTTON_COUNT);
    if ((key >= XUD_INPUT_KEY_FIRST) && (key <= XUD_INPUT_KEY_LAST)) {
        keyStates[key - XUD_INPUT_KEY_FIRST] = state;
    }
}

static void xud_test_input_step(XudInput* input, int key, uint8_t state, double cursorX, double cursorY) {
    uint8_t keyStates[XUD_INPUT_KEY_COUNT];
    uint8_t buttonStates[XUD_INPUT_BUTTON_COUNT];

    xud_test_input_snapshot(keyStates, buttonStates, key, state);
    (void)xud_input_advance(input, keyStates, buttonStates, cursorX, cursorY);
}

static void xud_test_input_pure(void) {
    XudInput* input = NULL;
    XudInput* fresh = NULL;
    XudContext* context = NULL;
    XudWindow* syntheticWindow = NULL;
    XudInput* opened = NULL;
    uint8_t keyStates[XUD_INPUT_KEY_COUNT];
    uint8_t buttonStates[XUD_INPUT_BUTTON_COUNT];
    bool flag = false;
    int token = 0;
    float move = 0.0f;
    const char* name = NULL;
    XudVector2 position = { 0.0f, 0.0f };

    /* The mirror the header's static_assert pins: the public tokens, GLFW's numbering and this rung's
       row count are one numbering. The assert is compile-time, so the check here is that the count
       the rows are sized by is the range the queries range-check against - the same expression, stat
       ed as a fact rather than only as an array size.
    */
    xud_check((XUD_INPUT_KEY_LAST - XUD_INPUT_KEY_FIRST + 1) == (int)XUD_INPUT_KEY_COUNT, "input/key-range-matches-count",
        "the rows are sized by the range the public queries range-check");
    xud_check((XUD_INPUT_BUTTON_LAST - XUD_INPUT_BUTTON_FIRST + 1) == (int)XUD_INPUT_BUTTON_COUNT, "input/button-range-matches-count",
        "the button rows are sized the same way");
    xud_check(XUD_INPUT_STATE_UP == GLFW_RELEASE, "input/up-is-glfw-release", "the state names are GLFW's, not a re-encoding");

    input = calloc(1, sizeof(XudInput));
    xud_check(input != NULL, "input/synthetic-alloc", "the rung must allocate");
    if (input == NULL) {
        return;
    }
    input->scaleX = 1.0;
    input->scaleY = 1.0;
    input->exitKey = XUD_KEY_ESCAPE;

    /* The opening state, read through the public queries above a synthetic shell: nothing held, noth
       ing queued, the exit key the rung opens with. The shell borrows the rung - neither owns the oth
       er here, and both are freed at the end of this half.
    */
    context = calloc(1, sizeof(XudContext));
    xud_check(context != NULL, "input/synthetic-shell", "the shell must allocate");
    if (context == NULL) {
        free(input);
        return;
    }
    context->input = input;

    xud_check(xud_is_key_down(&flag, context, XUD_KEY_A) == XUD_OK, "input/query-ok-headless", "a query reads records, no window needed");
    xud_check(flag == false, "input/opening-state-up", "a fresh rung holds nothing");
    xud_check(xud_get_exit_key(&token, context) == XUD_OK, "input/exit-key-readable", "the default is readable, not only conventional");
    xud_check(token == XUD_KEY_ESCAPE, "input/exit-key-default-escape", "raylib's SetExitKey says ESC to start with");
    xud_check(xud_get_key_pressed(&token, context) == XUD_OK, "input/queue-drain-empty-ok", "an empty queue answers 0, not a refusal");
    xud_check(token == 0, "input/queue-empty-reads-zero", "raylib's GetKeyPressed returns 0 when the queue is empty");
    xud_check(xud_get_char_pressed(&token, context) == XUD_OK, "input/char-queue-empty-ok", "same, for the codepoints");
    xud_check(token == 0, "input/char-queue-empty-reads-zero", "and 0 means drained");

    /* The guards, before the state: a token outside GLFW's queryable range is a bad argument rather
       than an answer about a row that does not exist - XUD_KEY_NULL names "no key", and the range i
       s the rows' range.
    */
    xud_check(xud_is_key_down(&flag, NULL, XUD_KEY_A) == XUD_INVALID_ARG, "input/null-context-refused", "nothing is silently ignored");
    xud_check(
        xud_is_key_down(&flag, context, XUD_KEY_NULL) == XUD_INVALID_ARG, "input/null-key-refused", "XUD_KEY_NULL is not a key to query");
    xud_check(xud_is_key_down(&flag, context, XUD_KEY_SPACE - 1) == XUD_INVALID_ARG, "input/below-range-refused",
        "GLFW's key state starts at the printable range");
    xud_check(
        xud_is_key_down(&flag, context, XUD_KEY_KB_MENU + 1) == XUD_INVALID_ARG, "input/above-range-refused", "and ends at GLFW_KEY_LAST");
    xud_check(xud_is_key_down(NULL, context, XUD_KEY_A) == XUD_INVALID_ARG, "input/null-out-refused",
        "an out-param that is not there is refused");
    xud_check(xud_is_mouse_button_down(&flag, context, XUD_MOUSE_BUTTON_LEFT) == XUD_OK, "input/button-query-ok",
        "the button rows are queried the same way");
    xud_check(flag == false, "input/button-open-up", "a fresh rung holds no button");
    xud_check(xud_is_mouse_button_down(&flag, context, XUD_INPUT_BUTTON_LAST + 1) == XUD_INVALID_ARG, "input/button-range-refused",
        "GLFW's button state is eight wide");
    xud_check(xud_set_mouse_scale(context, 0.0f, 1.0f) == XUD_INVALID_ARG, "input/zero-scale-refused", "the position queries divide by it");
    xud_check(xud_set_mouse_scale(context, (float)NAN, 1.0f) == XUD_INVALID_ARG, "input/nan-scale-refused",
        "and a non-finite one is refused too");
    xud_check(xud_set_exit_key(context, XUD_KEY_NULL) == XUD_OK, "input/no-exit-key-allowed", "raylib's SetExitKey takes 0 for none");
    xud_check(xud_get_exit_key(&token, context) == XUD_OK, "input/no-exit-key-reads-back", "and reads back");
    xud_check(token == 0, "input/no-exit-key-is-zero", "none is none");
    xud_check(xud_set_exit_key(context, XUD_KEY_NULL - 1) == XUD_INVALID_ARG, "input/bad-exit-key-refused",
        "a token that is not a key and not none");

    /* The two calls that need a window this synthetic rung does not have: the refusal is the guard,
       and it is XUD_UNSUPPORTED rather than a bad argument because nothing about the request was
       wrong - there is just no window system here to ask.
    */
    xud_check(xud_get_key_name(&name, context, XUD_KEY_A) == XUD_UNSUPPORTED, "input/keyname-no-window", "no layout to resolve a name on");
    xud_check(xud_set_mouse_position(context, 10, 10) == XUD_UNSUPPORTED, "input/set-cursor-no-window", "no window to place a cursor in");
    xud_check(xud_input_poll(input) == XUD_UNSUPPORTED, "input/poll-no-window", "the thin half above GLFW refuses without a window");

    /* The transition table, one key through a whole press-hold-repeat-release-press sequence. The
       states are the snapshot's, the answers are the rung's, and the pair is one boundary between
       two polls rather than two moments - which is why a key held across frames answers pressed ONCE
       at the first frame of the hold and never at the ones below it.
    */
    xud_test_input_step(input, XUD_KEY_A, XUD_INPUT_STATE_DOWN, 10.0, 20.0);
    xud_check(xud_is_key_pressed(&flag, context, XUD_KEY_A) == XUD_OK, "input/press-answers", "the query ran");
    xud_check(flag == true, "input/press-once", "a key that went down at this poll is pressed once");
    xud_check(xud_is_key_down(&flag, context, XUD_KEY_A) == XUD_OK, "input/down-answers", "the query ran");
    xud_check(flag == true, "input/down-while-held", "and it is held");
    xud_check(xud_is_key_pressed_repeat(&flag, context, XUD_KEY_A) == XUD_OK, "input/repeat-answers", "the query ran");
    xud_check(flag == false, "input/first-frame-not-repeat", "the first frame of a hold is not a repeat");
    xud_check(xud_is_key_released(&flag, context, XUD_KEY_A) == XUD_OK, "input/release-answers", "the query ran");
    xud_check(flag == false, "input/not-released-while-held", "a held key is not released");
    xud_check(xud_is_key_up(&flag, context, XUD_KEY_A) == XUD_OK, "input/up-answers", "the query ran");
    xud_check(flag == false, "input/not-up-while-held", "and not up");
    xud_check(input->keyQueueCount == 1u, "input/press-queues-one", "the press went into the queue");
    xud_check(xud_get_key_pressed(&token, context) == XUD_OK, "input/queue-reads-back", "the drain ran");
    xud_check(token == XUD_KEY_A, "input/queue-reads-the-key", "in the order the scan found it");
    xud_check(input->keyQueueCount == 0u, "input/queue-drained", "and the drain emptied it");

    xud_test_input_step(input, XUD_KEY_A, XUD_INPUT_STATE_DOWN, 10.0, 20.0);
    xud_check(xud_is_key_pressed(&flag, context, XUD_KEY_A) == XUD_OK, "input/hold-answers", "the query ran");
    xud_check(flag == false, "input/hold-not-pressed", "the second frame of a hold is not a press");
    xud_check(xud_is_key_down(&flag, context, XUD_KEY_A) == XUD_OK, "input/hold-down-answers", "the query ran");
    xud_check(flag == true, "input/hold-still-down", "and still held");

    xud_test_input_step(input, XUD_KEY_A, XUD_INPUT_STATE_REPEAT, 10.0, 20.0);
    xud_check(xud_is_key_pressed_repeat(&flag, context, XUD_KEY_A) == XUD_OK, "input/repeat-query-answers", "the query ran");
    xud_check(flag == true, "input/repeat-when-glfw-repeats", "a key GLFW answers REPEAT for is repeating");
    xud_check(xud_is_key_pressed(&flag, context, XUD_KEY_A) == XUD_OK, "input/repeat-press-answers", "the query ran");
    xud_check(flag == false, "input/repeat-not-pressed", "a repeating key was not pressed at this poll");
    xud_check(xud_is_key_down(&flag, context, XUD_KEY_A) == XUD_OK, "input/repeat-down-answers", "the query ran");
    xud_check(flag == true, "input/repeat-still-down", "and still held - REPEAT is a held state");

    xud_test_input_step(input, XUD_KEY_A, XUD_INPUT_STATE_UP, 10.0, 20.0);
    xud_check(xud_is_key_released(&flag, context, XUD_KEY_A) == XUD_OK, "input/release-query-answers", "the query ran");
    xud_check(flag == true, "input/released-once", "a key that came up at this poll is released once");
    xud_check(xud_is_key_up(&flag, context, XUD_KEY_A) == XUD_OK, "input/up-query-answers", "the query ran");
    xud_check(flag == true, "input/up-after-release", "and up");
    xud_check(xud_is_key_down(&flag, context, XUD_KEY_A) == XUD_OK, "input/down-query-answers", "the query ran");
    xud_check(flag == false, "input/not-down-after-release", "and not held");
    xud_check(input->keyQueueCount == 0u, "input/release-queues-nothing", "only a press into held goes into the queue");

    xud_test_input_step(input, XUD_KEY_A, XUD_INPUT_STATE_DOWN, 10.0, 20.0);
    xud_check(xud_is_key_pressed(&flag, context, XUD_KEY_A) == XUD_OK, "input/repress-answers", "the query ran");
    xud_check(flag == true, "input/repress-is-press", "a key that came up and went down again is pressed again");
    xud_check(xud_get_key_pressed(&token, context) == XUD_OK, "input/repress-drained",
        "the press above queued itself, so drain before counting on");
    xud_check(token == XUD_KEY_A, "input/repress-queue-is-a", "and it is A");

    /* The same table for a button, and the queue's order: two keys pressed between the same two pol
       ls come back in token order, because the scan that fills the queue is by token index.
    */
    xud_test_input_snapshot(keyStates, buttonStates, XUD_KEY_B, XUD_INPUT_STATE_UP);
    buttonStates[XUD_MOUSE_BUTTON_LEFT - XUD_INPUT_BUTTON_FIRST] = XUD_INPUT_STATE_DOWN;
    xud_check(xud_input_advance(input, keyStates, buttonStates, 10.0, 20.0) == XUD_OK, "input/button-advance-ok", "the snapshot ran");
    xud_check(xud_is_mouse_button_pressed(&flag, context, XUD_MOUSE_BUTTON_LEFT) == XUD_OK, "input/button-press-answers", "the query ran");
    xud_check(flag == true, "input/button-pressed", "the button went down at this poll");
    xud_check(xud_is_mouse_button_up(&flag, context, XUD_MOUSE_BUTTON_RIGHT) == XUD_OK, "input/button-up-answers", "the query ran");
    xud_check(flag == true, "input/other-button-up", "and the one nobody touched is up");

    xud_test_input_snapshot(keyStates, buttonStates, XUD_KEY_B, XUD_INPUT_STATE_UP);
    xud_check(xud_input_advance(input, keyStates, buttonStates, 10.0, 20.0) == XUD_OK, "input/button-up-advance-ok", "the button came up");
    xud_check(
        xud_is_mouse_button_released(&flag, context, XUD_MOUSE_BUTTON_LEFT) == XUD_OK, "input/button-release-answers", "the query ran");
    xud_check(flag == true, "input/button-released-once", "released once, at the poll that let go");

    xud_test_input_snapshot(keyStates, buttonStates, XUD_KEY_A, XUD_INPUT_STATE_DOWN);
    keyStates[XUD_KEY_B - XUD_INPUT_KEY_FIRST] = XUD_INPUT_STATE_DOWN;
    xud_check(xud_input_advance(input, keyStates, buttonStates, 10.0, 20.0) == XUD_OK, "input/two-key-advance-ok", "two keys in one frame");
    xud_check(input->keyQueueCount == 2u, "input/two-key-queue", "both presses queued");
    xud_check(xud_get_key_pressed(&token, context) == XUD_OK, "input/two-key-first", "the drain ran");
    xud_check(token == XUD_KEY_A, "input/two-key-token-order", "token order, because the scan is by token index");
    xud_check(xud_get_key_pressed(&token, context) == XUD_OK, "input/two-key-second", "the drain ran again");
    xud_check(token == XUD_KEY_B, "input/two-key-second-is-b", "and the second is B");
    xud_check(xud_get_key_pressed(&token, context) == XUD_OK, "input/two-key-drained", "the third call drains");
    xud_check(token == 0, "input/two-key-drained-zero", "and answers 0");

    /* The queue's bound, asserted rather than argued: every key down in one snapshot queues every ke
       y, and the count is exactly the row count - which is the proof beside XUD_INPUT_KEY_QUEUE_MAX
       in the header, read as a fact.
    */
    /* The queue's bound, asserted rather than argued: every key down in one snapshot on a fresh run
       g queues every key, and the count is exactly the row count - which is the proof beside XUD_IN
       PUT_KEY_QUEUE_MAX in the header, read as a fact. A fresh rung is used so the count is the ste
       p's own rather than the whole sequence's.
    */
    fresh = calloc(1, sizeof(XudInput));
    xud_check(fresh != NULL, "input/fresh-rung-for-bound", "the fresh rung must allocate");
    if (fresh != NULL) {
        memset(keyStates, XUD_INPUT_STATE_DOWN, sizeof(keyStates));
        memset(buttonStates, XUD_INPUT_STATE_UP, sizeof(buttonStates));
        xud_check(xud_input_advance(fresh, keyStates, buttonStates, 0.0, 0.0) == XUD_OK, "input/all-key-advance-ok",
            "every key down in one frame");
        xud_check(fresh->keyQueueCount == (uint32_t)XUD_INPUT_KEY_COUNT, "input/queue-bound-holds",
            "one press per key per poll, so the bound is the key count");
        free(fresh);
    }

    /* The snapshot's own guard: a byte outside GLFW's three states is refused BEFORE any row is writ
       ten, so a refused advance changes nothing rather than half a frame's state.
    */
    xud_test_input_snapshot(keyStates, buttonStates, XUD_KEY_A, XUD_INPUT_STATE_DOWN);
    keyStates[XUD_KEY_C - XUD_INPUT_KEY_FIRST] = 3u;
    xud_check(xud_input_advance(input, keyStates, buttonStates, 999.0, 999.0) == XUD_INVALID_ARG, "input/bad-state-refused",
        "a byte GLFW does not answer is refused");
    xud_check(input->cursorRawX == 10.0, "input/refused-advance-changed-nothing", "the cursor did not move on a refused snapshot");
    xud_check(input->keyState[XUD_KEY_A - XUD_INPUT_KEY_FIRST] == XUD_INPUT_STATE_DOWN, "input/refused-advance-kept-rows",
        "and the rows are the ones the last accepted frame left");
    xud_check(xud_input_advance(NULL, keyStates, buttonStates, 0.0, 0.0) == XUD_INVALID_ARG, "input/null-rung-refused",
        "the pure half guards its own receiver");

    /* The cursor: the delta is this poll's raw position against the poll below it, and the transform
       above it is raylib's own - offset first, then scale.
    */
    xud_test_input_step(input, XUD_KEY_A, XUD_INPUT_STATE_DOWN, 13.0, 25.0);
    xud_check(xud_get_mouse_delta(&position, context) == XUD_OK, "input/delta-query-ok", "the query ran");
    xud_check((position.x == 3.0f) && (position.y == 5.0f), "input/delta-between-polls", "13,25 against 10,20 - raylib's GetMouseDelta");
    xud_check(xud_get_mouse_position(&position, context) == XUD_OK, "input/position-query-ok", "the query ran");
    xud_check((position.x == 13.0f) && (position.y == 25.0f), "input/position-identity-transform", "offset zero, scale one");
    xud_check(xud_set_mouse_offset(context, 100, 0) == XUD_OK, "input/offset-set-ok", "raylib's SetMouseOffset");
    xud_check(xud_set_mouse_scale(context, 1.0f, 2.0f) == XUD_OK, "input/scale-set-ok", "raylib's SetMouseScale");
    xud_check(xud_get_mouse_position(&position, context) == XUD_OK, "input/transformed-query-ok", "the query ran");
    xud_check((position.x == -87.0f) && (position.y == 12.5f), "input/transform-applied",
        "(13-100)/1 and 25/2 - the pair above the raw position");
    xud_check(xud_get_mouse_x(&token, context) == XUD_OK, "input/mouse-x-query-ok", "raylib's GetMouseX");
    xud_check(token == -87, "input/mouse-x-int", "the pixel grid a caller places a shape on");
    xud_check(xud_get_mouse_y(&token, context) == XUD_OK, "input/mouse-y-query-ok", "raylib's GetMouseY");
    xud_check(token == 12, "input/mouse-y-int", "truncated, not rounded - the rung's cursor is a double");

    /* The wheel: the callbacks accumulate, the advance settles, and a caller reads the frame's whole
       movement rather than its last one.
    */
    input->wheelPendingY = 3.0;
    input->wheelPendingX = -1.0;
    xud_test_input_step(input, XUD_KEY_A, XUD_INPUT_STATE_DOWN, 13.0, 25.0);
    xud_check(xud_get_mouse_wheel_move_v(&position, context) == XUD_OK, "input/wheel-v-query-ok", "raylib's GetMouseWheelMoveV");
    xud_check((position.x == -1.0f) && (position.y == 3.0f), "input/wheel-accumulated", "both axes, sign kept");
    xud_check((input->wheelPendingX == 0.0) && (input->wheelPendingY == 0.0), "input/wheel-accumulator-reset",
        "the next frame starts from nothing");
    xud_check(xud_get_mouse_wheel_move(&move, context) == XUD_OK, "input/wheel-query-ok", "raylib's GetMouseWheelMove");
    xud_check(move == 3.0f, "input/wheel-larger-axis", "whichever axis moved more, by absolute value");

    /* The codepoints: the callbacks fill the queue, and the drain above it reports a cut list once
       when it is done rather than answering 0 as if nothing more arrived.
    */
    input->charQueue[0] = 'h';
    input->charQueue[1] = 'i';
    input->charQueueCount = 2u;
    xud_check(xud_get_char_pressed(&token, context) == XUD_OK, "input/char-first-ok", "the drain ran");
    xud_check(token == 'h', "input/char-first-is-h", "in the order the callbacks queued them");
    xud_check(xud_get_char_pressed(&token, context) == XUD_OK, "input/char-second-ok", "the drain ran again");
    xud_check(token == 'i', "input/char-second-is-i", "and the second");
    xud_check(xud_get_char_pressed(&token, context) == XUD_OK, "input/char-drained-ok", "the third call drains");
    xud_check(token == 0, "input/char-drained-zero", "and answers 0");
    input->charQueueCount = 0u;
    input->charTruncated = 1u;
    xud_check(xud_get_char_pressed(&token, context) == XUD_OUT_OF_RESOURCES, "input/char-truncation-reported",
        "a cut list is a report, not a silent truncation");
    xud_check(xud_get_char_pressed(&token, context) == XUD_OK, "input/char-truncation-once", "reported once, then the queue is just empty");

    /* The rung's own guards, headless: a window with no GLFW window behind it cannot carry the run
       g's callbacks, and a rung whose window is gone closes without touching GLFW.
    */
    syntheticWindow = calloc(1, sizeof(XudWindow));
    xud_check(syntheticWindow != NULL, "input/synthetic-window", "the window must allocate");
    if (syntheticWindow != NULL) {
        syntheticWindow->surface = VK_NULL_HANDLE;
        syntheticWindow->surfaceInstance = VK_NULL_HANDLE;
        xud_check(xud_input_open(&opened, syntheticWindow) == XUD_UNSUPPORTED, "input/open-no-glfw-window",
            "no window system to register callbacks on");
        xud_check(opened == NULL, "input/open-refusal-hands-back-nothing", "a refused open leaves no rung");
        xud_check(
            xud_input_open(NULL, syntheticWindow) == XUD_INVALID_ARG, "input/open-null-out-refused", "the guard runs before GLFW is asked");
        xud_check(xud_input_open(&opened, NULL) == XUD_INVALID_ARG, "input/open-null-window-refused", "and a window that is not there");
        free(syntheticWindow);
    }
    xud_check(xud_input_close(&input) == XUD_OK, "input/close-ok", "the rung closes");
    xud_check(input == NULL, "input/close-clears-handle", "and clears the handle it was given, so a second close is XUD_ALREADY");
    xud_check(xud_input_close(&input) == XUD_ALREADY, "input/close-twice-already", "a rung already gone is not a second teardown");
    free(context);
}

/*-----------------------------------------------------------------------------------------------
 * The GPU half: a real window, opened by the context this machine's Wayland session hosts
 *
 * The context is the only way this half reaches a real GLFW window, and it is the only thing that
 * opens one: xud_init_context() opens the input rung as part of bring-up, so the rung this half rea
 * ds is the context's own rather than one this half built. The checks are the records read back aft
 * er one real poll - the payload slot, the exit key, a key nobody pressed, the cursor inside the ex
 * tent, the name the layout resolves - and the two answers this backend gives that no headless run
 * can: the cursor placement GLFW's Wayland half refuses, and the payload slot a second rung cannot
 * take a second time.
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_input_gpu(void) {
    XudContext* context = NULL;
    XudInput* second = NULL;
    XudResult result = XUD_INVALID_ARG;
    bool flag = false;
    int token = 0;
    float move = 0.0f;
    const char* name = NULL;

    result = xud_init_context(&context, 64u, 64u);
    xud_check(result == XUD_OK, "input/gpu-context", "bring-up opened the rung as part of it");
    if (result != XUD_OK) {
        return;
    }
    xud_check(context->input != NULL, "input/gpu-rung-exists", "the rung is a bring-up requirement, not an optional field");

    /* The payload slot: the two callbacks GLFW calls find the rung through it, so the slot has to ho
       ld the rung the context owns - and a second rung cannot take it a second time, which is the
       branch the headless half above cannot reach.
    */
    xud_check(glfwGetWindowUserPointer(context->window->glfwWindow) == (void*)context->input, "input/gpu-payload-slot",
        "GLFW's own slot for this window names this rung");
    xud_check(xud_input_open(&second, context->window) == XUD_INTERNAL_ERROR, "input/gpu-slot-taken",
        "a second rung on one window is not a state this tree can be in");
    xud_check(second == NULL, "input/gpu-slot-refusal-clean", "and the refusal left nothing behind");

    /* One real poll, through the rung the context owns: the answer is XUD_OK and the records below a
       re what this session's window state says.
    */
    xud_check(xud_input_poll(context->input) == XUD_OK, "input/gpu-poll-ok", "one poll of a real window");
    xud_check(xud_is_key_down(&flag, context, XUD_KEY_A) == XUD_OK, "input/gpu-query-ok", "the public query read the real records");
    xud_check(flag == false, "input/gpu-no-stray-press", "nobody pressed A on this session, and the rung says so rather than guessing");
    xud_check(xud_get_exit_key(&token, context) == XUD_OK, "input/gpu-exit-key", "the default survived bring-up");
    xud_check(token == XUD_KEY_ESCAPE, "input/gpu-exit-key-escape", "ESC, raylib's default");
    xud_check(xud_get_key_name(&name, context, XUD_KEY_A) == XUD_OK, "input/gpu-keyname-ok", "the query ran on a live window");
    xud_check(name != NULL, "input/gpu-keyname-a-resolved", "the layout resolves a name for KEY_A on this session");
    xud_check(xud_get_mouse_x(&token, context) == XUD_OK, "input/gpu-mouse-x-ok", "the cursor position read back");
    xud_check((token >= 0) && ((uint32_t)token <= context->window->width), "input/gpu-mouse-in-window",
        "a cursor outside the window's extent is not this rung's answer");
    xud_check(xud_get_mouse_wheel_move(&move, context) == XUD_OK, "input/gpu-wheel-query-ok", "the wheel read back");
    xud_check(move == 0.0f, "input/gpu-wheel-still", "nobody scrolled this session, and the rung says so");

    /* The refusal this backend gives and no headless run can: GLFW's Wayland half will not place a
       cursor it cannot see outside the window (wl_window.c 2970), so the answer is XUD_UNSUPPORTED
       and the cursor is where it was, not where the caller asked.
    */
    xud_check(xud_set_mouse_position(context, 5, 5) == XUD_UNSUPPORTED, "input/gpu-set-cursor-refused",
        "GLFW's Wayland half refuses to place a cursor");
    xud_check(xud_get_mouse_x(&token, context) == XUD_OK, "input/gpu-set-cursor-read-back", "the query ran");
    xud_check(token != 5, "input/gpu-cursor-did-not-move", "and nothing moved to where it was asked");

    xud_close_context(&context);
    xud_check(context == NULL, "input/gpu-context-closed", "and the teardown cleared it");
}

void xud_test_input(void) {
    xud_test_input_pure();
    if (xud_gpu_tests_enabled()) {
        xud_test_input_gpu();
    }
}
