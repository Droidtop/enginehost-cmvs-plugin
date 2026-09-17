/*
 * THE IN-GAME TOOLBAR, headless.
 *
 * intproc.ps3 registers twelve interrupt procedures and one of them is the bar
 * across the top of every scene - SYSTEM, SAVE, LOAD, QUICK SAVE, QUICK LOAD,
 * AUTO MODE, TEXT SKIP, PREVIOUS CHOICE, NEXT CHOICE, VOICE PLAYBACK, DYNAMIC
 * TEXT BOX, PAUSE GAME. It builds itself out of the LAYER'S OWN sprite table
 * (scene.h) and it asks about thirty questions a frame about the state of the
 * engine before it decides what each icon looks like and whether a press
 * counts. This test plays the game to a scene and then presses the bar.
 *
 * Nothing here is hard-coded to a number the game happens to use: the checks
 * are that the bar BUILT itself (twelve wide icons and two narrow ones, all
 * inside the strip it declared, none overlapping), that the icon under the
 * pointer wears a different face from the rest, that a press on AUTO MODE and
 * on TEXT SKIP flips the engine switch that command owns, that a press on
 * SYSTEM leaves the story script for intproc.ps3's own screen, and that no
 * press advances the message underneath. The last check is the one the whole
 * item is about: every per-frame question the procedure asks is answered, so
 * the procedure runs to its end instead of stopping at the first stale sys[0].
 *
 * Needs the game; with none on this machine it says so and passes, the same
 * shape as the archive and save tests.
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

/* Advance one frame with a tap in the message window, which is how a reader
 * gets through the prologue. */
static int read_on(cmvs_session *s, int frames)
{
    char err[256];
    int f;
    for (f = 0; f < frames; f++) {
        cmvs_session_pointer(s, 640, 402);
        cmvs_session_button(s, 0, 1);
        cmvs_session_button(s, 0, 0);
        if (cmvs_session_frame(s, err, sizeof err) <= 0) return 0;
    }
    return 1;
}

/* Hold the pointer still for a few frames, which is what a player does before
 * a press and what the bar needs to notice the hover. */
static void hover(cmvs_session *s, int x, int y, int frames)
{
    char err[256];
    int f;
    for (f = 0; f < frames; f++) {
        cmvs_session_pointer(s, x, y);
        cmvs_session_frame(s, err, sizeof err);
    }
}

static void press(cmvs_session *s, int x, int y)
{
    char err[256];
    int f;
    cmvs_session_pointer(s, x, y);
    cmvs_session_button(s, 0, 1);
    for (f = 0; f < 3; f++) {
        cmvs_session_pointer(s, x, y);
        cmvs_session_frame(s, err, sizeof err);
    }
    cmvs_session_button(s, 0, 0);
    for (f = 0; f < 20; f++) {
        cmvs_session_pointer(s, x, y);
        cmvs_session_frame(s, err, sizeof err);
    }
}

/* A fresh session read far enough into the story for the bar to be up. */
static cmvs_session *into_the_scene(const char *game, const char *saves)
{
    char err[256] = {0};
    cmvs_session *s = cmvs_session_open(game, NULL, NULL, saves, err, sizeof err);
    if (!s) { printf("  (cannot open %s: %s)\n", game, err); return NULL; }
    if (!read_on(s, 900)) { cmvs_session_close(s); return NULL; }
    return s;
}

/*
 * Every per-frame question the toolbar procedure asks, with the routine each
 * one is. A run that reaches the end of the procedure calls all of them; one
 * that stops early does not, and one that answers a stale sys[0] gets the
 * wrong shape of bar. Both halves are checked: called, and answered.
 */
static const struct { int command; const char *what; } QUERIES[] = {
    { 0x130, "0x004661d0  how many lines the backlog holds" },
    { 0x14b, "0x00466c50  is the layer there" },
    { 0x14d, "0x00466c90  has the layer been taken away" },
    { 0x15f, "0x00467290  is the pointer inside this rectangle" },
    { 0x18d, "0x00467be0  where and how opaque the object is" },
    { 0x194, "0x00468810  which sprite the pointer is on" },
    { 0x195, "0x00468870  is this sprite registered" },
    { 0x196, "0x004688d0  wear this state" },
    { 0x1a8, "0x00468b60  KEY_FUNCTION_07, hide the window" },
    { 0x1aa, "0x00468ba0  KEY_FUNCTION_10, auto advance" },
    { 0x1ae, "0x00468c20  KEY_FUNCTION_12, replay the voice" },
    { 0x226, "0x0046a3e0  how much of the screen shake is left" },
    { 0x2ce, "0x0047234c  the flag at +0x293c" },
    { 0x2e5, "0x0046b8a0  the flag at +0x5d8, which AUTO MODE writes" },
    { 0x2eb, "0x0046baf0  the skip flag at +0x5b8" },
    { 0x2ee, "0x00472498  whether +0x634 is set" },
    { 0x34e, "0x00449000  the device code at +0x56c" },
    { 0x353, "0x0046e7e0  the gesture recogniser's +0x2c" },
    { 0x359, "0x0046e8c0  and what it is drawing" },
    { 0x362, "0x0046e900  and whether it is drawing at all" },
};

static void test_toolbar(const char *game, const char *saves)
{
    cmvs_session *s = into_the_scene(game, saves);
    int i, wide = 0, narrow = 0, registered = 0, bad = 0, overlap = 0;
    int rx[CMVS_LAYER_SPRITES], rw[CMVS_LAYER_SPRITES];
    int rh[CMVS_LAYER_SPRITES], ry[CMVS_LAYER_SPRITES];
    int worn[CMVS_LAYER_SPRITES], alone[CMVS_LAYER_SPRITES];
    int hovered = -1, differs = 0;

    printf("the in-game toolbar, in %s\n", game);
    if (!s) { check(0, "the game opens and the prologue runs"); return; }
    printf("        read to %s, %ld lines\n",
           cmvs_session_script(s), cmvs_session_messages(s));

    for (i = 0; i < CMVS_LAYER_SPRITES; i++) {
        if (!cmvs_session_sprite(s, TOOLBAR_LAYER, i, &rx[i], &ry[i],
                                 &rw[i], &rh[i], &worn[i])) {
            rw[i] = 0;
            continue;
        }
        registered++;
        if (rw[i] > 60) wide++; else narrow++;
        /* the strip the procedure itself declares with 0x15f: 1280 x 54 at the
         * layer's origin */
        if (rx[i] < 0 || ry[i] < 0 || rx[i] + rw[i] > 1280 || ry[i] + rh[i] > 54) bad++;
    }
    printf("        %d sprites registered on layer %d, %d wide and %d narrow\n",
           registered, TOOLBAR_LAYER, wide, narrow);
    check(registered > 0, "the bar registers its sprites at all");
    check(wide == 12, "twelve wide icons, one per named button");
    check(narrow == 2, "and the two narrow ones between them");
    check(bad == 0, "every icon is inside the strip the bar declares");

    for (i = 0; i < CMVS_LAYER_SPRITES; i++) {
        int j;
        if (!rw[i]) continue;
        for (j = i + 1; j < CMVS_LAYER_SPRITES; j++) {
            if (!rw[j]) continue;
            if (rx[i] < rx[j] + rw[j] && rx[j] < rx[i] + rw[i]
                && ry[i] < ry[j] + rh[j] && ry[j] < ry[i] + rh[i]) overlap++;
        }
    }
    check(overlap == 0, "and no two of them can be pressed at once");

    /* Point at the first icon and see the bar answer. Every icon is asked what
     * it is wearing with the pointer away from the bar and again with it on
     * the first icon; exactly the one under the pointer should change. */
    hover(s, 640, 402, 4);
    for (i = 0; i < CMVS_LAYER_SPRITES; i++)
        cmvs_session_sprite(s, TOOLBAR_LAYER, i, NULL, NULL, NULL, NULL, &alone[i]);
    if (registered) {
        for (i = 0; i < CMVS_LAYER_SPRITES; i++) if (rw[i]) { hovered = i; break; }
    }
    if (hovered >= 0) {
        hover(s, rx[hovered] + rw[hovered] / 2, ry[hovered] + rh[hovered] / 2, 4);
        for (i = 0; i < CMVS_LAYER_SPRITES; i++) {
            int now = 0;
            if (!rw[i]) continue;
            cmvs_session_sprite(s, TOOLBAR_LAYER, i, NULL, NULL, NULL, NULL, &now);
            if (now != alone[i]) differs++;
        }
        printf("        icon %d wears %d alone and the pointer changes %d icon(s)\n",
               hovered, alone[hovered], differs);
    }
    check(differs == 1, "pointing at an icon changes that icon and no other");

    cmvs_session_close(s);
}

/* One press per fresh session, because each one leaves the engine somewhere
 * else and the next check wants the scene back. */
static void test_press(const char *game, const char *saves)
{
    cmvs_session *s;
    int i, x[CMVS_LAYER_SPRITES], y[CMVS_LAYER_SPRITES];
    int w[CMVS_LAYER_SPRITES], h[CMVS_LAYER_SPRITES], n = 0;
    int order[CMVS_LAYER_SPRITES];
    long before;
    int was, now;

    s = into_the_scene(game, saves);
    if (!s) { check(0, "the game opens for the press checks"); return; }
    for (i = 0; i < CMVS_LAYER_SPRITES; i++)
        if (cmvs_session_sprite(s, TOOLBAR_LAYER, i, &x[i], &y[i], &w[i], &h[i], NULL)
            && w[i] > 60)
            order[n++] = i;
    cmvs_session_close(s);
    if (n < 12) { check(0, "the bar is built before a press is tried"); return; }

    /* The bar reads left to right in the order it registers: SYSTEM, SAVE,
     * LOAD, QUICK SAVE, QUICK LOAD, AUTO MODE, then TEXT SKIP and the rest. */
    {
        int auto_icon = order[5], skip_icon = order[6], system_icon = order[0];

        s = into_the_scene(game, saves);
        if (!s) { check(0, "a session for AUTO MODE"); return; }
        was = cmvs_session_switch(s, CMVS_SWITCH_AUTO);
        before = cmvs_session_messages(s);
        hover(s, x[auto_icon] + w[auto_icon] / 2, y[auto_icon] + h[auto_icon] / 2, 4);
        press(s, x[auto_icon] + w[auto_icon] / 2, y[auto_icon] + h[auto_icon] / 2);
        now = cmvs_session_switch(s, CMVS_SWITCH_AUTO);
        printf("        AUTO MODE: %d before, %d after, %ld lines before and %ld after\n",
               was, now, before, cmvs_session_messages(s));
        check(now != was, "pressing AUTO MODE flips the flag command 0x2e5 owns");
        check(cmvs_session_messages(s) == before,
              "and the press does not fall through and advance the message");
        cmvs_session_close(s);

        s = into_the_scene(game, saves);
        if (!s) { check(0, "a session for TEXT SKIP"); return; }
        was = cmvs_session_switch(s, CMVS_SWITCH_SKIP);
        before = cmvs_session_messages(s);
        hover(s, x[skip_icon] + w[skip_icon] / 2, y[skip_icon] + h[skip_icon] / 2, 4);
        press(s, x[skip_icon] + w[skip_icon] / 2, y[skip_icon] + h[skip_icon] / 2);
        now = cmvs_session_switch(s, CMVS_SWITCH_SKIP);
        printf("        TEXT SKIP: %d before, %d after\n", was, now);
        check(now != was, "pressing TEXT SKIP flips the skip flag at +0x5b8");
        cmvs_session_close(s);

        s = into_the_scene(game, saves);
        if (!s) { check(0, "a session for SYSTEM"); return; }
        before = cmvs_session_messages(s);
        hover(s, x[system_icon] + w[system_icon] / 2, y[system_icon] + h[system_icon] / 2, 4);
        press(s, x[system_icon] + w[system_icon] / 2, y[system_icon] + h[system_icon] / 2);
        printf("        SYSTEM: now in %s, %ld lines\n",
               cmvs_session_script(s), cmvs_session_messages(s));
        check(strcmp(cmvs_session_script(s), "intproc.ps3") == 0,
              "pressing SYSTEM runs intproc.ps3's own screen");
        check(cmvs_session_messages(s) == before,
              "and that press does not advance the message either");
        cmvs_session_close(s);
    }
}

static void test_queries(const char *game, const char *saves)
{
    cmvs_session *s = into_the_scene(game, saves);
    size_t q;
    int missing = 0, silent = 0;
    if (!s) { check(0, "the game opens for the query census"); return; }
    /* a few frames with the pointer on the bar, so the branches that only run
     * under the pointer run too */
    hover(s, 40, 26, 8);
    for (q = 0; q < sizeof QUERIES / sizeof QUERIES[0]; q++) {
        int known = 0;
        long calls = cmvs_session_command_calls(s, QUERIES[q].command, &known);
        if (!calls) { silent++; printf("  NOT ASKED  0x%03x %s\n", QUERIES[q].command, QUERIES[q].what); }
        else if (!known) { missing++; printf("  UNANSWERED 0x%03x %s\n", QUERIES[q].command, QUERIES[q].what); }
    }
    check(silent == 0, "the procedure asks every question it is meant to");
    check(missing == 0, "and this engine answers all of them");
    cmvs_session_close(s);
}

int main(int argc, char **argv)
{
    int i;
    const char *saves = "/tmp/cmvs-toolbar-test";
    if (argc < 2) {
        printf("no game folder given; nothing to check\n");
        printf("\n%d checks, %d failed\n", checks, failures);
        return 0;
    }
    for (i = 1; i < argc; i++) {
        test_toolbar(argv[i], saves);
        test_press(argv[i], saves);
        test_queries(argv[i], saves);
    }
    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
