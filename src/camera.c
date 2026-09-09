#include "camera.h"

/*
 * The defaults are the constructor's at 0x004437c0. They matter: a script that
 * never configures a camera still draws through one, and the screen size it
 * assumes (1024 x 614) is not the game's.
 */
void cmvs_camera_init(cmvs_camera *c)
{
    if (!c) return;
    c->kind = 1;
    c->x = 0.0f;
    c->y = 3.0f;
    c->z = 0.0f;
    c->reference_depth = 10.0f;
    c->view_width = 10.0f;
    c->view_height = 6.0f;
    c->screen_width = 1024.0f;
    c->screen_height = 614.0f;
    c->lift = 24.0f;
    c->roll = 0.0f;
    c->spin = 0.0f;
    c->centre_x = 512.0f;
    c->centre_y = 307.0f;
    c->aspect_x = 1.0f;
    c->aspect_y = 1.0f;
    c->mode = 0;
    c->alternate = 0;
}

/*
 * 0x00443da0, the branch its first field selects with `dec eax` three times:
 * kind 1 is at 0x00444069, kind 2 at 0x00443f65 and kind 3 - the one every
 * ChronoClock script asks for, with 0x05f (3, camera) - at 0x00443dc2. Only
 * kind 3 is written here; the other two answer 0 so the scene falls back to
 * the flat placement rather than inventing a projection.
 */
int cmvs_camera_project(const cmvs_camera *c, const cmvs_placement *p,
                        cmvs_projection *out)
{
    float dz, scale, kx, ky, across, down;

    if (!c || !p || !out) return 0;
    if (c->kind != 3) return 0;

    /*
     * 0x00443dc5: the depth in front of the camera, and 0x00443dd4 throws the
     * item away when it is not more than nine units out. The constant is the
     * engine's, not a game's.
     */
    dz = p->z - c->z;
    if (!(dz > 9.0f)) return 0;

    /*
     * 0x00443df3 splits on the camera's roll: with no roll the whole first
     * term drops out and the scale is the item's own depth over the real one,
     * which is 1 for anything a script places at the depth it stands at. The
     * rolled branch is at 0x00443dfe and has not been read yet.
     */
    if (c->roll != 0.0f) return 0;
    scale = p->plane / dz;

    /* 0x00443e36: a fade over the last unit before the front plane. */
    out->alpha = dz >= 10.0f ? 255 : (int) ((dz - 9.0f) * 256.0f);
    if (out->alpha < 0) out->alpha = 0;
    if (out->alpha > 255) out->alpha = 255;

    /*
     * 0x00443e5f: how many pixels one world unit covers at this depth. The
     * camera says how wide the world is at its reference depth (0x058), so the
     * factor falls off with distance exactly as the item's own scale does.
     */
    across = c->view_width * dz / c->reference_depth;
    down = c->view_height * dz / c->reference_depth;
    if (across == 0.0f || down == 0.0f) return 0;
    if (c->aspect_x == 0.0f || c->aspect_y == 0.0f) return 0;
    kx = c->screen_width / across / c->aspect_x;
    ky = c->screen_height / down / c->aspect_y;

    /*
     * 0x00443e7f and 0x00443e97. The horizontal is the item's offset from the
     * camera's axis; the vertical is the camera's height ABOVE the item, so
     * the world's y grows upwards while the screen's grows down. Both land
     * against the principal point, which command 0x059 keeps at the middle of
     * the screen - and that is the whole answer to where a staged background
     * sits.
     */
    out->x = kx * scale * (p->x - c->x) + c->centre_x;
    out->y = ky * (scale * (c->y - p->y) + p->lift) + c->centre_y
             + (p->plane - dz) * c->lift;
    out->scale_x = p->scale_x * scale;
    out->scale_y = p->scale_y * scale;
    return 1;
}
