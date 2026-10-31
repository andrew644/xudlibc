/***********************************************************************************************
 *
 *   xud_input.h - the input rung: one GLFW window's key/mouse state, kept as a frame-to-frame
 *                 state machine the public input API reads
 *
 *   Owns : XudInput - the state above ONE GLFW window: the per-key and per-button state rows the
 *          window answers (GLFW's own GLFW_RELEASE / GLFW_PRESS / GLFW_REPEAT, kept here as this
 *          rung's 0 / 1 / 2), the frame-to-frame transitions raylib's IsKeyPressed/IsKeyDown/IsKe
 *          yUp family reads out of a pair of those rows, the key and char queues GLFW's key and ch
 *          ar callbacks fill, the scroll GLFW's scroll callback accumulates between polls, the cu
 *          rsor position plus raylib's offset and scale above it, and the exit key raylib's SetEx
 *          itKey records - plus the rungs that bring it up (xud_input_open(), which registers GLF
 *          W's char and scroll callbacks on the window) and down (xud_input_close()), and the one
 *          pure half the whole rung's correctness lives in: xud_input_advance(), which moves the
 *          state machine one frame given a snapshot of what the window answers.
 *   Layer: above the window (it reads that window's GLFW state and nothing else - no Vulkan symb
 *          ol, no loader row, no Vk anything) and below xud.h's public input calls, which are thi
 *          s rung's state machine plus the one context guard. It is the input half of the design
 *          sketch's bottom box ("GLFW window (extent, input, framebuffer-size)") and, beside the
 *          window layer, the only place in this tree that talks to GLFW.
 *
 *   Why the state machine is a separate, pure half: GLFW's state is only readable through a wind
 *          ow that exists, and a window exists only on a machine that has a window system - so a
 *          check that "a key held at the last poll and not held at this one reads as released" co
 *          uld never run on a machine with no device present if that fact were only stated inside
 *          the poll. xud_input_advance() takes the snapshot as an ARRAY OF BYTES rather than as a
 *          window, so the transition table, the queues it fills and the cursor delta it computes
 *          are all answerable headlessly (tests/xud_test_input.c drives it with hand-written sna
 *          pshots), and xud_input_poll() is the thin half above it: read GLFW into a snapshot, hand
 *          it to advance. The public queries read the same records through a shell context, so th
 *          eir half is headless too.
 *
 *   What this layer does NOT own: the window (the window layer, below it - this rung only borrows
 *          the GLFWwindow* it names), the frame loop that polls it (xud_core.c's public frame pair
 *          is what runs one poll per frame), the gamepad and touch halves of raylib's input API (G
 *          LFW's joystick/gamepad rung is a separate subsystem and GLFW's Wayland backend has no
 *          touch events at all - wl_window.c registers pointer MOTION, BUTTON and AXIS listeners
 *          and no touch listener), and the cursor SHAPE (raylib's SetMouseCursor would need a GLF
 *          Wcursor this rung owns and destroys, and GLFW's own table calls four of the ten standa
 *          rd shapes only "Maybe" on a Wayland theme).
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#ifndef XUD_XUD_INPUT_H_
#define XUD_XUD_INPUT_H_

#include "xud.h" /* Required for: XudResult and the public XUD_KEY_* / XUD_MOUSE_BUTTON_* tokens this rung's queries take and range-check against */
#include "xud_window.h" /* Required for: XudWindow, the window whose GLFWwindow* the poll reads */
#include <GLFW/glfw3.h> /* LAST of the block: its Vulkan declarations are inside #if defined(VK_VERSION_1_0), and xud_window.h above has already seen <vulkan/vulkan.h> - see that header's include-order note. Included again here on purpose: this rung's constants ARE GLFW's key and button tokens. */
#include <assert.h> /* Required for: static_assert, the compile-time checks below */
#include <stdint.h> /* Required for: uint8_t (the state rows), int32_t (the queues' element) */
#include <string.h> /* Required for: memset, the fresh rung's zeroed state rows */

/*-----------------------------------------------------------------------------------------------
 * Four facts this rung's contract rests on, all compile-time
 *-----------------------------------------------------------------------------------------------*/

/* The state rows are indexed by (token - first token), so the range below has to be the range GLF
   W's own key and button state arrays are - glfwGetKey refuses a key below GLFW_KEY_SPACE or above
   GLFW_KEY_LAST (input.c 749-756) and glfwGetMouseButton refuses a button outside the same kind of
   range (input.c 757-768), so a rung that tracked a wider range would be reading past what GLFW st
   ores, and one that tracked a narrower one would be answering "up" for keys the window is actual
   ly holding.
*/
#define XUD_INPUT_KEY_FIRST GLFW_KEY_SPACE
#define XUD_INPUT_KEY_LAST GLFW_KEY_LAST
#define XUD_INPUT_KEY_COUNT (GLFW_KEY_LAST - GLFW_KEY_SPACE + 1)

#define XUD_INPUT_BUTTON_FIRST GLFW_MOUSE_BUTTON_1
#define XUD_INPUT_BUTTON_LAST GLFW_MOUSE_BUTTON_LAST
#define XUD_INPUT_BUTTON_COUNT (GLFW_MOUSE_BUTTON_LAST - GLFW_MOUSE_BUTTON_1 + 1)

static_assert((XUD_INPUT_KEY_COUNT > 0) && (XUD_INPUT_KEY_COUNT < 1024),
    "xud_input.h: the key range GLFW's state array covers is a positive count this rung sizes its "
    "rows by - and it is small enough that the poll's one scan of it is a scan, not a table");

static_assert((XUD_INPUT_BUTTON_COUNT > 0) && (XUD_INPUT_BUTTON_COUNT <= 16u),
    "xud_input.h: GLFW's mouse-button range is a handful of buttons, so the button rows are two "
    "tiny arrays beside the key rows rather than a thing that needs its own allocator");

/* The state this rung keeps is GLFW's own answer stored as its own byte: glfwGetKey returns the
   window's keys[key] verbatim (input.c 757) and GLFW's three states are 0/1/2 in the order below,
   so the poll can store what GLFW answered without a conversion - and GLFW's sticky mode (a fourth
   value, _GLFW_STICK) is never asked for here because nothing in this tree calls glfwSetStickyKey
   sInputMode, which is why "down" below is written as "not up" rather than as "== GLFW_PRESS".
*/
#define XUD_INPUT_STATE_UP GLFW_RELEASE
#define XUD_INPUT_STATE_DOWN GLFW_PRESS
#define XUD_INPUT_STATE_REPEAT GLFW_REPEAT

static_assert((GLFW_RELEASE == 0) && (GLFW_PRESS == 1) && (GLFW_REPEAT == 2),
    "xud_input.h: the rung stores GLFW's answer verbatim, so its own three state names are GLF"
    "W's three numbers and not a mapping this layer invented");

/* The key queue's bound is the key count, and that is a proof rather than a hope: advance() pushes
   at most one entry per key per poll (a key only transitions into "down" once between two polls),
   so a queue that holds one entry per key can never be overflowed and no key press can be dropped.
   The char queue has no such bound - GLFW's Wayland backend re-emits a char for a key held down ac
   ross polls (wl_window.c 1459, the keyRepeatScancode path), so the count between two polls is un
   bounded and the overflow is recorded and reported rather than silently cut.
*/
#define XUD_INPUT_KEY_QUEUE_MAX XUD_INPUT_KEY_COUNT
#define XUD_INPUT_CHAR_QUEUE_MAX 64u

static_assert(XUD_INPUT_KEY_QUEUE_MAX == XUD_INPUT_KEY_COUNT,
    "xud_input.h: one press per key per poll, one slot per key - the key queue cannot overflow");

/*-----------------------------------------------------------------------------------------------
 * The mirror this rung's tokens are pinned against
 *
 * xud.h names raylib's key and mouse-button tokens (reference/raylib/raylib.h 587-720) as its own
 * XUD_KEY_* / XUD_MOUSE_BUTTON_* macros, with numeric values - and the state this rung reads is GL
 * F W's, indexed by GLFW's tokens. The equality below is what makes the two the same numbering: a
 * public XUD_KEY_LEFT is the index GLFW's state array uses, and if the public mirror ever drifts -
 * one token copied with the wrong value, one name renamed - this is a compile error rather than a
 * key that answers "up" while the window is holding it.
 *-----------------------------------------------------------------------------------------------*/

static_assert((XUD_KEY_NULL == 0) && (XUD_KEY_APOSTROPHE == GLFW_KEY_APOSTROPHE) && (XUD_KEY_COMMA == GLFW_KEY_COMMA)
        && (XUD_KEY_MINUS == GLFW_KEY_MINUS) && (XUD_KEY_PERIOD == GLFW_KEY_PERIOD) && (XUD_KEY_SLASH == GLFW_KEY_SLASH)
        && (XUD_KEY_ZERO == GLFW_KEY_0) && (XUD_KEY_ONE == GLFW_KEY_1) && (XUD_KEY_TWO == GLFW_KEY_2) && (XUD_KEY_THREE == GLFW_KEY_3)
        && (XUD_KEY_FOUR == GLFW_KEY_4) && (XUD_KEY_FIVE == GLFW_KEY_5) && (XUD_KEY_SIX == GLFW_KEY_6) && (XUD_KEY_SEVEN == GLFW_KEY_7)
        && (XUD_KEY_EIGHT == GLFW_KEY_8) && (XUD_KEY_NINE == GLFW_KEY_9) && (XUD_KEY_SEMICOLON == GLFW_KEY_SEMICOLON)
        && (XUD_KEY_EQUAL == GLFW_KEY_EQUAL) && (XUD_KEY_A == GLFW_KEY_A) && (XUD_KEY_B == GLFW_KEY_B) && (XUD_KEY_C == GLFW_KEY_C)
        && (XUD_KEY_D == GLFW_KEY_D) && (XUD_KEY_E == GLFW_KEY_E) && (XUD_KEY_F == GLFW_KEY_F) && (XUD_KEY_G == GLFW_KEY_G)
        && (XUD_KEY_H == GLFW_KEY_H) && (XUD_KEY_I == GLFW_KEY_I) && (XUD_KEY_J == GLFW_KEY_J) && (XUD_KEY_K == GLFW_KEY_K)
        && (XUD_KEY_L == GLFW_KEY_L) && (XUD_KEY_M == GLFW_KEY_M) && (XUD_KEY_N == GLFW_KEY_N) && (XUD_KEY_O == GLFW_KEY_O)
        && (XUD_KEY_P == GLFW_KEY_P) && (XUD_KEY_Q == GLFW_KEY_Q) && (XUD_KEY_R == GLFW_KEY_R) && (XUD_KEY_S == GLFW_KEY_S)
        && (XUD_KEY_T == GLFW_KEY_T) && (XUD_KEY_U == GLFW_KEY_U) && (XUD_KEY_V == GLFW_KEY_V) && (XUD_KEY_W == GLFW_KEY_W)
        && (XUD_KEY_X == GLFW_KEY_X) && (XUD_KEY_Y == GLFW_KEY_Y) && (XUD_KEY_Z == GLFW_KEY_Z)
        && (XUD_KEY_LEFT_BRACKET == GLFW_KEY_LEFT_BRACKET) && (XUD_KEY_BACKSLASH == GLFW_KEY_BACKSLASH)
        && (XUD_KEY_RIGHT_BRACKET == GLFW_KEY_RIGHT_BRACKET) && (XUD_KEY_GRAVE == GLFW_KEY_GRAVE_ACCENT)
        && (XUD_KEY_SPACE == GLFW_KEY_SPACE) && (XUD_KEY_ESCAPE == GLFW_KEY_ESCAPE) && (XUD_KEY_ENTER == GLFW_KEY_ENTER)
        && (XUD_KEY_TAB == GLFW_KEY_TAB) && (XUD_KEY_BACKSPACE == GLFW_KEY_BACKSPACE) && (XUD_KEY_INSERT == GLFW_KEY_INSERT)
        && (XUD_KEY_DELETE == GLFW_KEY_DELETE) && (XUD_KEY_RIGHT == GLFW_KEY_RIGHT) && (XUD_KEY_LEFT == GLFW_KEY_LEFT)
        && (XUD_KEY_DOWN == GLFW_KEY_DOWN) && (XUD_KEY_UP == GLFW_KEY_UP) && (XUD_KEY_PAGE_UP == GLFW_KEY_PAGE_UP)
        && (XUD_KEY_PAGE_DOWN == GLFW_KEY_PAGE_DOWN) && (XUD_KEY_HOME == GLFW_KEY_HOME) && (XUD_KEY_END == GLFW_KEY_END)
        && (XUD_KEY_CAPS_LOCK == GLFW_KEY_CAPS_LOCK) && (XUD_KEY_SCROLL_LOCK == GLFW_KEY_SCROLL_LOCK)
        && (XUD_KEY_NUM_LOCK == GLFW_KEY_NUM_LOCK) && (XUD_KEY_PRINT_SCREEN == GLFW_KEY_PRINT_SCREEN) && (XUD_KEY_PAUSE == GLFW_KEY_PAUSE)
        && (XUD_KEY_F1 == GLFW_KEY_F1) && (XUD_KEY_F2 == GLFW_KEY_F2) && (XUD_KEY_F3 == GLFW_KEY_F3) && (XUD_KEY_F4 == GLFW_KEY_F4)
        && (XUD_KEY_F5 == GLFW_KEY_F5) && (XUD_KEY_F6 == GLFW_KEY_F6) && (XUD_KEY_F7 == GLFW_KEY_F7) && (XUD_KEY_F8 == GLFW_KEY_F8)
        && (XUD_KEY_F9 == GLFW_KEY_F9) && (XUD_KEY_F10 == GLFW_KEY_F10) && (XUD_KEY_F11 == GLFW_KEY_F11) && (XUD_KEY_F12 == GLFW_KEY_F12)
        && (XUD_KEY_LEFT_SHIFT == GLFW_KEY_LEFT_SHIFT) && (XUD_KEY_LEFT_CONTROL == GLFW_KEY_LEFT_CONTROL)
        && (XUD_KEY_LEFT_ALT == GLFW_KEY_LEFT_ALT) && (XUD_KEY_LEFT_SUPER == GLFW_KEY_LEFT_SUPER)
        && (XUD_KEY_RIGHT_SHIFT == GLFW_KEY_RIGHT_SHIFT) && (XUD_KEY_RIGHT_CONTROL == GLFW_KEY_RIGHT_CONTROL)
        && (XUD_KEY_RIGHT_ALT == GLFW_KEY_RIGHT_ALT) && (XUD_KEY_RIGHT_SUPER == GLFW_KEY_RIGHT_SUPER) && (XUD_KEY_KB_MENU == GLFW_KEY_MENU)
        && (XUD_KEY_KP_0 == GLFW_KEY_KP_0) && (XUD_KEY_KP_1 == GLFW_KEY_KP_1) && (XUD_KEY_KP_2 == GLFW_KEY_KP_2)
        && (XUD_KEY_KP_3 == GLFW_KEY_KP_3) && (XUD_KEY_KP_4 == GLFW_KEY_KP_4) && (XUD_KEY_KP_5 == GLFW_KEY_KP_5)
        && (XUD_KEY_KP_6 == GLFW_KEY_KP_6) && (XUD_KEY_KP_7 == GLFW_KEY_KP_7) && (XUD_KEY_KP_8 == GLFW_KEY_KP_8)
        && (XUD_KEY_KP_9 == GLFW_KEY_KP_9) && (XUD_KEY_KP_DECIMAL == GLFW_KEY_KP_DECIMAL) && (XUD_KEY_KP_DIVIDE == GLFW_KEY_KP_DIVIDE)
        && (XUD_KEY_KP_MULTIPLY == GLFW_KEY_KP_MULTIPLY) && (XUD_KEY_KP_SUBTRACT == GLFW_KEY_KP_SUBTRACT)
        && (XUD_KEY_KP_ADD == GLFW_KEY_KP_ADD) && (XUD_KEY_KP_ENTER == GLFW_KEY_KP_ENTER) && (XUD_KEY_KP_EQUAL == GLFW_KEY_KP_EQUAL)
        && (XUD_MOUSE_BUTTON_LEFT == GLFW_MOUSE_BUTTON_LEFT) && (XUD_MOUSE_BUTTON_RIGHT == GLFW_MOUSE_BUTTON_RIGHT)
        && (XUD_MOUSE_BUTTON_MIDDLE == GLFW_MOUSE_BUTTON_MIDDLE) && (XUD_MOUSE_BUTTON_SIDE == GLFW_MOUSE_BUTTON_4)
        && (XUD_MOUSE_BUTTON_EXTRA == GLFW_MOUSE_BUTTON_5) && (XUD_MOUSE_BUTTON_FORWARD == GLFW_MOUSE_BUTTON_6)
        && (XUD_MOUSE_BUTTON_BACK == GLFW_MOUSE_BUTTON_7),
    "xud.h's key and mouse-button mirror is GLFW's numbering, token for token - the public tokens "
    "index this rung's state rows, so a drift is a wrong answer at runtime and a compile error here");

/*-----------------------------------------------------------------------------------------------
 * The rung
 *-----------------------------------------------------------------------------------------------*/

typedef struct XudInput {
    XudWindow* window; /* the window whose GLFW state the poll reads - BORROWED, the window layer owns it */

    /* The state machine's two rows per key and per button: the current frame's and the frame below
       it. A transition is a pair read together, which is why both rows are kept rather than a fla
       g per key.
    */
    uint8_t keyState[XUD_INPUT_KEY_COUNT];
    uint8_t keyPrevious[XUD_INPUT_KEY_COUNT];
    uint8_t buttonState[XUD_INPUT_BUTTON_COUNT];
    uint8_t buttonPrevious[XUD_INPUT_BUTTON_COUNT];

    /* What the last poll saw press, in the order the scan found it, and what GLFW's char callback
       queued since the poll below it. The key queue's bound is the proof above; the char queue's
       overflow is recorded and reported by the query that drains it.
    */
    int32_t keyQueue[XUD_INPUT_KEY_QUEUE_MAX];
    uint32_t keyQueueCount;
    int32_t charQueue[XUD_INPUT_CHAR_QUEUE_MAX];
    uint32_t charQueueCount;
    uint32_t charTruncated;

    /* raylib's SetExitKey: the key that ends a run, ESC to start with. This rung records it; the
       caller asks for it and acts - the library never exits a process of its own. XUD_KEY_NULL (0)
       means no exit key.
    */
    int32_t exitKey;

    /* The cursor, as the last poll read it and as the caller's offset and scale above it read it.
       The raw pair is what GLFW answered; the delta is that pair read against the poll below it,
       which is the "between frames" of raylib's GetMouseDelta.
    */
    double cursorRawX;
    double cursorRawY;
    double cursorX;
    double cursorY;
    double deltaCursorX;
    double deltaCursorY;

    /* Scroll, in the two halves of a frame: what the callbacks accumulated since the poll below it
       (still arriving while a frame is drawn) and what the poll above it saw, which is what a cal
       ler reads.
    */
    double wheelPendingX;
    double wheelPendingY;
    double wheelX;
    double wheelY;

    /* raylib's SetMouseOffset / SetMouseScale, above the raw position. A scale of zero is refused
       at the setter, so the division below never reads one.
    */
    double offsetX;
    double offsetY;
    double scaleX;
    double scaleY;
} XudInput;

/*-----------------------------------------------------------------------------------------------
 * The rungs
 *-----------------------------------------------------------------------------------------------*/

/// Bring the rung up above a live window: one host allocation, GLFW's char and scroll callbacks re
/// gistered on that window (they are the only way GLFW's Wayland backend delivers a codepoint -
/// wl_window.c 1372 - or a wheel/trackpad scroll - wl_window.c 1605 - to anything), and GLFW's wind
/// ow user pointer set to this rung so the two callbacks can find it (nothing else in this tree ta
/// kes that slot, and the check below is what says so). Refuses XUD_INVALID_ARG on a NULL out-param
/// or a non-null one on entry or a NULL window; XUD_UNSUPPORTED on a window with no GLFW window be
/// hind it; XUD_INTERNAL_ERROR when the window's payload slot is already taken.
XudResult xud_input_open(XudInput** outInput, XudWindow* window);

/// One frame's poll: glfwPollEvents() (GLFW's Wayland backend delivers a key event only when its ev
/// ent loop runs, and this is the only call in this tree that runs it) then the window's whole key
/// and button state read into a snapshot and handed to the pure half below. XUD_INVALID_ARG on a NU
/// LL rung; XUD_UNSUPPORTED on a rung whose window is gone; the pure half's codes otherwise.
XudResult xud_input_poll(XudInput* input);

/// The pure half above the snapshot: move the state machine one frame given the states the window
/// answers for this one, its cursor position as GLFW read it, and the scroll its callbacks have al
/// ready accumulated. Fills the transitions, the queues, the delta and the wheel rows, and touches
/// nothing else. XUD_INVALID_ARG on a NULL rung, a NULL snapshot row or a state byte outside GLF
/// W's three; XUD_OK otherwise. This is the half the headless test drives.
XudResult xud_input_advance(XudInput* input, const uint8_t keyStates[XUD_INPUT_KEY_COUNT],
    const uint8_t buttonStates[XUD_INPUT_BUTTON_COUNT], double cursorRawX, double cursorRawY);

/// Tear the rung down: GLFW's two callbacks unregistered and the window's user pointer cleared wh
/// ile the GLFW window is still alive (the window layer's own close is what destroys it, and a cal
/// lback naming a freed rung is not something this leaves behind), then the rung freed. XUD_ALREAD
/// Y on a NULL field, XUD_INVALID_ARG on a NULL out-param, and the rung's own window is never tou
/// ched.
XudResult xud_input_close(XudInput** input);

#endif /* XUD_XUD_INPUT_H_ */
