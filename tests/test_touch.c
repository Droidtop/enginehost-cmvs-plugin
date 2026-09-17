/*
 * THE ANDROID TOUCH PATH, headless.
 *
 * tests/test_toolbar.c presses the bar the way a MOUSE presses it: the pointer
 * is put on the icon, four frames run, and only then does the button go down.
 * A finger cannot do that. On a touch screen the position and the press arrive
 * together, in one ACTION_DOWN, and the rig proved the difference - every one
 * of the fourteen presses reached the engine at the right game coordinate and
 * not one was taken (DEFECT D, build 67 and build 68).
 *
 * So this fixture drives what the device drives, and through the same two
 * calls jni.c makes - cmvs_session_pointer and cmvs_session_button, never the
 * engine's own press - with a frame between events and never inside one,
 * because CmvsPlugin's frame loop and its touch events are both posted to the
 * main looper. The screen-to-game conversion is CmvsPlugin.ScreenView's own,
 * so a coordinate here is a coordinate on the rig's 1920 x 1080 instance.
 *
 * Needs the game; with none on this machine it says so and passes, the same
 * shape as the other fixtures.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "interp.h"
#include "scene.h"
#include "session.h"

static int failures;
static int checks;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) { failures++; printf("  FAIL  %s\n", what); }
    else printf("  ok    %s\n", what);
}

#define TOOLBAR_LAYER 7

/* The rig: a 1920 x 1080 view showing a 1280 x 720 picture, so scale 1.5 and
 * no margins. ScreenView.onTouchEvent's own arithmetic. */
#define VIEW_W 1920
#define VIEW_H 1080

typedef struct { cmvs_session *s; int w, h; } screen;

static float view_scale(const screen *v)
{
    float sx = (float) VIEW_W / v->w, sy = (float) VIEW_H / v->h;
    return sx < sy ? sx : sy;
}

static void view_to_game(const screen *v, int sx, int sy, int *gx, int *gy)
{
    float scale = view_scale(v);
    *gx = (int) ((sx - (VIEW_W - v->w * scale) / 2) / scale);
    *gy = (int) ((sy - (VIEW_H - v->h * scale) / 2) / scale);
}

static char err[256];

static void step(screen *v) { cmvs_session_frame(v->s, err, sizeof err); }

/* ACTION_DOWN: the position first, then the button, and the frame loop runs
 * afterwards - which is the order and the timing of the Java. */
static void action_down(screen *v, int sx, int sy)
{
    int gx, gy;
    view_to_game(v, sx, sy, &gx, &gy);
    cmvs_session_pointer(v->s, gx, gy);
    cmvs_session_button(v->s, 0, 1);
}

static void action_move(screen *v, int sx, int sy)
{
    int gx, gy;
    view_to_game(v, sx, sy, &gx, &gy);
    cmvs_session_pointer(v->s, gx, gy);
}

static void action_up(screen *v, int sx, int sy)
{
    int gx, gy;
    view_to_game(v, sx, sy, &gx, &gy);
    cmvs_session_pointer(v->s, gx, gy);
    cmvs_session_button(v->s, 0, 0);
}

/*
 * One tap of a finger, with the shape the rig's own evidence file recorded:
 * ACTION_DOWN, a run of ACTION_MOVEs a pixel or two apart while the finger
 * rests, then ACTION_UP. 300 ms is the hold that evidence timed, which is
 * about nineteen frames at the loop's 16 ms.
 */
static void finger_tap(screen *v, int sx, int sy, int hold_frames)
{
    int f;
    action_down(v, sx, sy);
    for (f = 0; f < hold_frames; f++) {
        action_move(v, sx + (f & 1), sy);
        step(v);
    }
    action_up(v, sx, sy);
    for (f = 0; f < 6; f++) step(v);
}

/* Reading on, the way a player does: a tap in the message window each time. */
static int read_on(screen *v, int lines)
{
    int i;
    for (i = 0; i < lines; i++) {
        action_down(v, 960, 603);
        step(v);
        action_up(v, 960, 603);
        step(v);
        if (cmvs_session_frame(v->s, err, sizeof err) <= 0) return 0;
    }
    return 1;
}

static cmvs_session *into_the_scene(const char *game, const char *saves, screen *v)
{
    cmvs_session *s = cmvs_session_open(game, NULL, NULL, saves, err, sizeof err);
    if (!s) { printf("  (cannot open %s: %s)\n", game, err); return NULL; }
    v->s = s;
    v->w = cmvs_session_width(s);
    v->h = cmvs_session_height(s);
    if (!read_on(v, 500)) { cmvs_session_close(s); return NULL; }
    return s;
}

static void test_touch(const char *game, const char *saves)
{
    screen v;
    int i, n = 0, x[CMVS_LAYER_SPRITES], y[CMVS_LAYER_SPRITES];
    int w[CMVS_LAYER_SPRITES], h[CMVS_LAYER_SPRITES];
    int icon = -1, layer = -1, was, now, events;
    long before;
    float scale;

    printf("the Android touch path, in %s\n", game);
    if (!into_the_scene(game, saves, &v)) {
        check(0, "the game opens and the prologue runs");
        return;
    }
    printf("        read to %s, %ld lines\n",
           cmvs_session_script(v.s), cmvs_session_messages(v.s));

    scale = view_scale(&v);
    for (i = 0; i < CMVS_LAYER_SPRITES; i++)
        if (cmvs_session_sprite(v.s, TOOLBAR_LAYER, i, &x[n], &y[n], &w[n], &h[n], NULL)
            && w[n] > 60)
            n++;
    check(n >= 12, "the toolbar is up before a finger is put on it");
    if (n < 12) { cmvs_session_close(v.s); return; }

    /*
     * AUTO MODE, the sixth wide icon, tapped ONCE with no hover of any kind:
     * the pointer is on the message window until the ACTION_DOWN lands on the
     * bar. This is the press the rig took fourteen of and the engine took none
     * of.
     */
    was = cmvs_session_switch(v.s, CMVS_SWITCH_AUTO);
    before = cmvs_session_messages(v.s);
    finger_tap(&v, (int) ((x[5] + w[5] / 2) * scale), (int) ((y[5] + h[5] / 2) * scale), 19);
    now = cmvs_session_switch(v.s, CMVS_SWITCH_AUTO);
    events = cmvs_session_icon_events(v.s, &icon, &layer);
    printf("        AUTO MODE %d -> %d, icon events %d (icon %d on layer %d), "
           "%ld lines before and %ld after\n",
           was, now, events, icon, layer, before, cmvs_session_messages(v.s));
    check(events > 0, "a tap with no hover before it is TAKEN by the sprite under it");
    check(icon == 5 && layer == TOOLBAR_LAYER,
          "and it is the sprite the finger landed on, on the bar's own layer");
    check(now != was, "the switch the icon owns is flipped");
    check(cmvs_session_messages(v.s) == before,
          "and the tap does not also advance the message underneath");
    cmvs_session_close(v.s);
}

/*
 * And the other half: a tap in the message window must still read on, and a
 * pad confirm - which carries no pointer at all - must not be held back.
 */
static void test_message_and_pad(const char *game, const char *saves)
{
    screen v;
    long before;
    int f;

    printf("a tap in the message window, and a pad confirm, in %s\n", game);
    if (!into_the_scene(game, saves, &v)) {
        check(0, "the game opens for the message checks");
        return;
    }
    before = cmvs_session_messages(v.s);
    /* A line is not always due on the first tap - the reveal has to finish
     * first - so this taps until it is, the way a reader does. */
    for (f = 0; f < 8 && cmvs_session_messages(v.s) == before; f++)
        finger_tap(&v, 960, 603, 4);
    printf("        a finger on the message window (screen 960,603 = game 640,402): %ld lines -> %ld\n",
           before, cmvs_session_messages(v.s));
    check(cmvs_session_messages(v.s) > before, "a tap in the message window reads on");

    before = cmvs_session_messages(v.s);
    for (f = 0; f < 8 && cmvs_session_messages(v.s) == before; f++) {
        int k;
        cmvs_session_button(v.s, 0, 1);
        for (k = 0; k < 3; k++) cmvs_session_frame(v.s, err, sizeof err);
        cmvs_session_button(v.s, 0, 0);
        for (k = 0; k < 3; k++) cmvs_session_frame(v.s, err, sizeof err);
    }
    printf("        a pad confirm with no pointer: %ld lines -> %ld\n",
           before, cmvs_session_messages(v.s));
    check(cmvs_session_messages(v.s) > before,
          "a confirm that moves no pointer is not held back at all");
    cmvs_session_close(v.s);
}

int main(int argc, char **argv)
{
    int i;
    const char *saves = "/tmp/cmvs-touch-test";
    if (argc < 2) {
        printf("no game folder given; nothing to check\n");
        printf("\n%d checks, %d failed\n", checks, failures);
        return 0;
    }
    for (i = 1; i < argc; i++) {
        test_touch(argv[i], saves);
        test_message_and_pad(argv[i], saves);
    }
    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
