#include "input.h"

static int usable(const cmvs_input *in, int function)
{
    return in && function > 0 && function < CMVS_FUNCTIONS;
}

void cmvs_input_move(cmvs_input *in, int x, int y)
{
    if (!in) return;
    in->x = x;
    in->y = y;
    in->have_pointer = 1;
}

void cmvs_input_function(cmvs_input *in, int function, int down)
{
    cmvs_function *f;
    if (!usable(in, function)) return;
    f = &in->fn[function];
    /*
     * The press and the release are separate edges, both latched until a
     * reader clears them, so a tap shorter than one frame still arrives. The
     * original gets the same effect from its frame counter at +0x570: PRESSED
     * is the frame the counter reads one and RELEASED the frame it falls back
     * to zero (0x00449020).
     */
    if (down && !f->held) f->pressed = 1;
    if (!down && f->held) f->released = 1;
    f->held = down ? 1 : 0;
}

void cmvs_input_button(cmvs_input *in, int button, int down)
{
    /* ChronoClock's key.cfg binds the left button to 決定 and the right to
     * キャンセル; a frontend with named buttons reaches the rest through
     * cmvs_input_function. */
    if (button == 0) cmvs_input_function(in, CMVS_FN_CONFIRM, down);
    else if (button == 1) cmvs_input_function(in, CMVS_FN_CANCEL, down);
}

void cmvs_input_navigate(cmvs_input *in, int direction)
{
    if (!in) return;
    if (direction < 0) in->fn[CMVS_FN_UP].pressed = 1;
    else if (direction > 0) in->fn[CMVS_FN_DOWN].pressed = 1;
}

int cmvs_input_released(const cmvs_input *in, int function, int *held)
{
    if (!usable(in, function)) { if (held) *held = 0; return 0; }
    if (held) *held = in->fn[function].held;
    return in->fn[function].released;
}

int cmvs_input_pressed(const cmvs_input *in, int function)
{
    return usable(in, function) ? in->fn[function].pressed : 0;
}

int cmvs_input_held(const cmvs_input *in, int function)
{
    return usable(in, function) ? in->fn[function].held : 0;
}

void cmvs_input_clear(cmvs_input *in, int function)
{
    if (!usable(in, function)) return;
    in->fn[function].released = 0;
    in->fn[function].pressed = 0;
}
