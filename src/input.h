/*
 * The pointer and the buttons, the way the engine's input device holds them.
 *
 * Everything here was read off the accessors the menu poll calls on the object
 * at +0xcb8 (0x00448B20 onwards). The device keeps, for each button, a HELD
 * level and two EDGES - one for the press and one for the release - and the
 * reader clears the edges once it has acted on them:
 *
 *   +0x43c  left released    +0x440  left held      +0x444  left pressed
 *   +0x448  right released   +0x44c  right held     +0x450  right pressed
 *   +0x454  up pressed       +0x458  up held
 *   +0x460  down pressed     +0x464  down held
 *   0x00448B50 clears the two left edges, 0x00448B90 the right ones,
 *   0x00448C20 the up edge, 0x00448C50 the down edge.
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

    int left_held, left_pressed, left_released;
    int right_held, right_pressed, right_released;
    int up_pressed, down_pressed;
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
