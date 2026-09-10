/*
 * The pointer and the engine's key functions, the way its input device holds them.
 *
 * These are not buttons. CMVS resolves every input through a table of 24 NAMED
 * FUNCTIONS, each with five alternative bindings read out of key.cfg's
 * [KEY_FUNCTION_01..24] (loader 0x0044CCD0, table at +0x31c, resolver
 * 0x00449020). A function's three flags live at +0x430 + 12*n:
 *
 *   n=1  +0x43c/+0x440/+0x444   KEY_FUNCTION_01  Confirm     (default left click, pad 1, Enter)
 *   n=2  +0x448/+0x44c/+0x450   KEY_FUNCTION_02  Cancel      (default right click, pad 2, Escape)
 *   n=3  +0x454/+0x458/+0x45c   KEY_FUNCTION_03  Cursor up   (default Up, pad up)
 *   n=4  +0x460/+0x464/+0x468   KEY_FUNCTION_04  Cursor down (default Down, pad down)
 *
 * in each triple: +0 RELEASED edge, +4 HELD level, +8 PRESSED edge. The
 * accessors the menu poll calls on the object at +0xcb8 start at 0x00448B20 and
 * run one pair per function in KEY_FUNCTION order; 0x00448B50 clears function
 * 01's two edges, 0x00448B90 function 02's, 0x00448C20 function 03's,
 * 0x00448C50 function 04's. That is why a tap and a pad press are the same
 * thing here: the original never distinguishes them either.
 *
 * The full 24-function table, its evidence and the 128-entry input-code space
 * are in coordination agents/cmvs/KEY-FUNCTIONS.md. Only these four are
 * implemented, because only these four have anything to act on yet.
 *
 * The pointer is in ENGINE coordinates - the game's own screen, 1280x720 for
 * ChronoClock - not in the window's. The original scales between the two
 * inside the menu poll, using the client size it was handed at bind time; here
 * the frontend does it, because the frontend is the only part that knows how
 * its picture is laid out on a real display. That is one conversion in one
 * place instead of the same one in the desktop runner, the JNI and the menus.
 */
#ifndef CMVS_INPUT_H
#define CMVS_INPUT_H

typedef struct {
    int x, y;                  /* the pointer, in engine coordinates */
    int have_pointer;          /* nothing has pointed at the screen yet */

    int confirm_held, confirm_pressed, confirm_released;
    int cancel_held, cancel_pressed, cancel_released;
    int cursor_up_pressed, cursor_down_pressed;
} cmvs_input;

/* What a frontend calls. `button` is 0 for the left button (a tap, confirm)
 * and 1 for the right one (cancel). */
void cmvs_input_move(cmvs_input *in, int x, int y);
void cmvs_input_button(cmvs_input *in, int button, int down);

/* A press of a direction: -1 up, +1 down. The menus move their selection on
 * these and warp the pointer to whatever they select, which is how a pad and a
 * pointer stay one mechanism rather than two. */
void cmvs_input_navigate(cmvs_input *in, int direction);

#endif
