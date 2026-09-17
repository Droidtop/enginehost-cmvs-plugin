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
 * KIND 1, at 0x00444069: the projection a camera has before a script gives it
 * one. It differs from kind 3 in every part, so it is its own routine.
 *
 * - it accepts anything IN FRONT of the camera (0x0044407a tests dz > 0),
 *   where kind 3 throws away anything nearer than nine units;
 * - its scale does not fall off with distance at all. It is a symmetric zoom
 *   about the item's own plane: t = (plane - dz) / 2 (the constant at
 *   0x005477f8 is 0.5), and the item is drawn at 1 + t in front of its plane
 *   and at 1 / (1 + |t|) behind it (0x004440c6 and 0x004440cc). An item
 *   standing at the depth it declares - which is every staged object
 *   ChronoClock places - is drawn at 1, once, not squared;
 * - the pixels one world unit covers come from the camera's REFERENCE depth
 *   over the item's depth (0x00444115 onwards), and the principal point is
 *   half the screen rather than command 0x059's centre;
 * - the fade is over the last half unit before the camera itself
 *   (0x004440ec: 0.5 at 0x005494fc, 512 at 0x00549a60), not over the unit
 *   before a front plane.
 *
 * The rotation the routine also answers (camera +0x38 times 100, modulo
 * 36000, at 0x004441c8) and the depth it writes at out+0x08 have nowhere to go
 * in this engine's projection yet, so they are not computed.
 */
static int project_flat(const cmvs_camera *c, const cmvs_placement *p,
                        cmvs_projection *out)
{
    float dz = p->z - c->z;
    float t, scale, unit, across, down;

    if (!(dz > 0.0f)) return 0;                     /* 0x0044407a */

    t = (p->plane - dz) * 0.5f;                     /* 0x004440ae */
    scale = t >= 0.0f ? 1.0f + t : 1.0f / (1.0f - t);

    /* 0x004440ec: 255 unless the item is within half a unit of the camera. */
    out->alpha = dz < 0.5f ? (int) (dz * 512.0f) : 255;
    if (out->alpha < 0) out->alpha = 0;
    if (out->alpha > 255) out->alpha = 255;

    if (c->view_width == 0.0f || c->view_height == 0.0f) return 0;
    unit = c->reference_depth / dz;                 /* 0x00444115 */
    across = c->screen_width / c->view_width;
    down = c->screen_height / c->view_height;

    out->x = (p->x - c->x) * unit * across + c->screen_width * 0.5f;
    out->y = ((c->y - p->y) * unit + p->lift) * down + c->screen_height * 0.5f
             + (p->plane - dz) * c->lift;
    out->scale_x = p->scale_x * scale;              /* 0x0044418c, once */
    out->scale_y = p->scale_y * scale;
    return 1;
}

/*
 * 0x00443da0, the branch its first field selects with `dec eax` three times:
 * kind 1 is at 0x00444069, kind 2 at 0x00443f65 and kind 3 - the one every
 * ChronoClock script asks for, with 0x05f (3, camera) - at 0x00443dc2.
 *
 * Kind 1 and kind 3 are both written here. Kind 1 is the camera EVERY camera
 * is until a script says otherwise (0x004437c0 stores 1 in the first field),
 * and it is reached in real play: a save carries the scene's objects but not
 * the scene's cameras, so a game loaded from the title draws snky01.ps3's
 * rooftop through camera 0 exactly as the constructor left it. Kind 2 is not
 * written: it is a longer routine that joins kind 3's tail at 0x00443ecf, and
 * nothing in ChronoClock reaches it - every 0x05f in the game's 84 scripts
 * asks for 3 - so it answers 0 rather than being half-read.
 */
int cmvs_camera_project(const cmvs_camera *c, const cmvs_placement *p,
                        cmvs_projection *out)
{
    float dz, scale, kx, ky, across, down;

    if (!c || !p || !out) return 0;
    if (c->kind == 1) return project_flat(c, p, out);
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
    /*
     * 0x00443eda, and it is the item's own scale times the perspective one
     * SQUARED, not once. The routine keeps scale * scale in [ebp+0xc] from
     * 0x00443e31 - `fld st(0)` then `fmul st(0), st(0)` - and multiplies both
     * of the item's scales by that, never by the single factor.
     *
     * The two agree exactly whenever the item stands at the depth it declares,
     * because then plane / dz is 1, and every ChronoClock scene read before
     * this one did stand there: the title, the prologue and the first rooftop
     * all place their camera at z = 0 with the background at z = 70 and a
     * plane of 70. So this made no difference to any of them - the reference
     * renders beside BRIEF.md are byte for byte what they were - and all the
     * difference in the world to a scene whose camera has moved in, where
     * ONCE leaves the background too small to cover the frame it has been
     * pushed across. snky02.ps3's rooftop pool is that scene: camera at
     * (-63, 36, 25), background at (0, 36, 70) anchored on its own centre,
     * so the anchor lands 1524 px right of the middle and only a bitmap drawn
     * at the squared scale is still wide enough to reach back over it.
     */
    out->scale_x = p->scale_x * scale * scale;
    out->scale_y = p->scale_y * scale * scale;
    return 1;
}
