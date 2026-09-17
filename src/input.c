#include "input.h"

static int usable(const cmvs_input *in, int function)
{
    return in && function > 0 && function < CMVS_FUNCTIONS;
}

void cmvs_input_move(cmvs_input *in, int x, int y)
{
    if (!in) return;
    if (!in->have_pointer || x != in->x || y != in->y) in->moved = 1;
    in->x = x;
    in->y = y;
    in->have_pointer = 1;
}

/* The raise itself, with no waiting: what the device poll hands the script. */
static void raise(cmvs_input *in, int function, int down)
{
    cmvs_function *f = &in->fn[function];
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

void cmvs_input_poll(cmvs_input *in)
{
    if (!in) return;
    in->moved = 0;
    if (in->queued <= 0) return;
    /*
     * The head waits out the poll that carries the move it arrived with - that
     * poll is the hover the script needs - and is raised by the one after it.
     * One transition per poll, oldest first, so a press and the release that
     * consumes its edge (0x00452b76) never land together either.
     */
    if (in->queue[0].wait > 0) { in->queue[0].wait--; return; }
    {
        int i;
        raise(in, in->queue[0].function, in->queue[0].down);
        for (i = 1; i < in->queued; i++) in->queue[i - 1] = in->queue[i];
        in->queued--;
    }
}

void cmvs_input_function(cmvs_input *in, int function, int down)
{
    if (!usable(in, function)) return;
    raise(in, function, down);
}

/*
 * A button that belongs to the POINTER - the mouse's own two, functions 01 and
 * 02 by ChronoClock's key.cfg. These are the ones that cannot legitimately
 * change in the same poll as a pointer move; see cmvs_input_poll.
 */
static void pointer_button(cmvs_input *in, int function, int down)
{
    if (!in->moved && in->queued == 0) { raise(in, function, down); return; }
    if (in->queued < CMVS_INPUT_QUEUE) {
        in->queue[in->queued].function = (unsigned char) function;
        in->queue[in->queued].down = (unsigned char) (down ? 1 : 0);
        in->queue[in->queued].wait = (unsigned char) (in->queued == 0 ? 1 : 0);
        in->queued++;
        return;
    }
    /*
     * More transitions have arrived between two polls than there are polls to
     * hand them out on - a caller pressing and releasing every frame, which no
     * real pointer does. Nothing is dropped: what is waiting is raised now, in
     * order, and the caller is back to the plain behaviour until it stops.
     */
    {
        int i;
        for (i = 0; i < in->queued; i++)
            raise(in, in->queue[i].function, in->queue[i].down);
        in->queued = 0;
    }
    raise(in, function, down);
}

void cmvs_input_button(cmvs_input *in, int button, int down)
{
    /* ChronoClock's key.cfg binds the left button to 決定 and the right to
     * キャンセル; a frontend with named buttons reaches the rest through
     * cmvs_input_function. */
    if (!in) return;
    if (button == 0) pointer_button(in, CMVS_FN_CONFIRM, down);
    else if (button == 1) pointer_button(in, CMVS_FN_CANCEL, down);
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
