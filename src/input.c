#include "input.h"

void cmvs_input_move(cmvs_input *in, int x, int y)
{
    if (!in) return;
    in->x = x;
    in->y = y;
    in->have_pointer = 1;
}

void cmvs_input_button(cmvs_input *in, int button, int down)
{
    if (!in) return;
    if (button == 0) {
        /* The press and the release are separate edges, both latched until a
         * reader clears them: a tap shorter than one frame still arrives. */
        if (down && !in->confirm_held) in->confirm_pressed = 1;
        if (!down && in->confirm_held) in->confirm_released = 1;
        in->confirm_held = down ? 1 : 0;
    } else if (button == 1) {
        if (down && !in->cancel_held) in->cancel_pressed = 1;
        if (!down && in->cancel_held) in->cancel_released = 1;
        in->cancel_held = down ? 1 : 0;
    }
}

void cmvs_input_navigate(cmvs_input *in, int direction)
{
    if (!in) return;
    if (direction < 0) in->cursor_up_pressed = 1;
    else if (direction > 0) in->cursor_down_pressed = 1;
}
