/*
 * .CMV VIDEO, headless.
 *
 * One scene of snky02.ps3 - the monorail ride - sets its background with
 * command 0x308 and a MOVIE instead of the usual 0x020 + 0x030 and a still, and
 * with that command missing the scene had no background at all for fifty
 * message lines. The codec behind it is CMVS's own: a "CMV9" container of JBPD
 * chunks, which is the JBP1 inside a PB3 wrapped differently (cmv.h says how,
 * and names the routine each part came out of).
 *
 * The checks are in three parts.
 *
 * THE CONTAINER AND THE BITSTREAM, on bg350a.cmv, the movie the defect is
 * about. The fields must agree with each other, every tile of every frame must
 * be the shape the picture gives it, and every one of the 73 frames must decode
 * consuming exactly the coefficients its own header declares and no more - for
 * 73 independently built Huffman streams that is a strong statement, because a
 * bitstream read even one bit wrong desynchronises within a macroblock or two
 * and then overruns. The MASK is checked both ways round as well, since reading
 * it inverted is the one mistake that still produces a picture: the intra must
 * lay down the 2979 static macroblocks and leave the 621 moving ones alone.
 *
 * THE PICTURE, against a still of the same background. bg400a.cmv in video.cpz
 * and bg400a.pb3 in bg.cpz are the same room rendered the same way, so the two
 * can be compared directly and the bound is a tight one. (bg350a has a still
 * too, and it is NOT usable as a reference: it is a separate render with the
 * carriage lit differently and its windows blown out white, and it correlates
 * at 0.60 where bg400a's correlates at 0.98. It is measured and printed here
 * anyway, because that difference is worth being able to see.)
 *
 * EVERY MOVIE THE GAME SHIPS, so that a container this transcription does not
 * cover is refused by name rather than decoded into noise.
 *
 * And then THE SCENE, if the reference saves are on this machine: the game is
 * played to the monorail, the movie must be live on the object the script
 * named, its frame must advance on the engine's own clock, and the frame above
 * the message window must not be black.
 *
 * Needs the game; with none on this machine it says so and passes, the same
 * shape as the archive, save and toolbar tests.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmv.h"
#include "game.h"
#include "interp.h"
#include "pb3.h"
#include "session.h"

static int failures;
static int checks;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) { failures++; printf("  FAIL  %s\n", what); }
    else printf("  ok    %s\n", what);
}

/* How black the frame above the message window is, in per cent of the pixels
 * sampled. This is the measure the defect was found with. */
static int black_fraction(const uint8_t *px, int w, int h)
{
    long total = 0, black = 0;
    int r, c;
    for (r = 0; r < h * 2 / 3; r += 4)
        for (c = 0; c < w; c += 4) {
            const uint8_t *q = px + 4 * ((size_t) r * w + c);
            total++;
            if (q[0] < 10 && q[1] < 10 && q[2] < 10) black++;
        }
    return total ? (int) (black * 100 / total) : 0;
}

/* A frame with NO press: the script stays on its message line and the movie
 * player is the only thing that moves. */
static int idle(cmvs_session *s, int frames)
{
    char err[256];
    int f;
    for (f = 0; f < frames; f++)
        if (cmvs_session_frame(s, err, sizeof err) <= 0) return 0;
    return 1;
}

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

/*
 * The worst of the three channels' correlation between the decoded surface and
 * a still, over the STATIC macroblocks - the part of the picture the movie's
 * own mask says never moves, which is the only part a single still can be
 * expected to agree with. The mean absolute difference comes back too, but only
 * to be printed: it is dominated by the difference in overall level and says
 * almost nothing, which two pictures of different rooms measuring the same 31.6
 * showed plainly.
 */
static double still_correlation(cmv_movie *m, const pb3_image *still, double *mad_out)
{
    const uint8_t *px = cmv_pixels(m);
    int stride = cmv_stride(m), gx, gy, r, c, k;
    double sa[3] = {0,0,0}, sb[3] = {0,0,0}, sd[3] = {0,0,0};
    double saa[3] = {0,0,0}, sbb[3] = {0,0,0}, sab[3] = {0,0,0};
    double worst = 1.0, mad = 0.0;
    long n = 0;
    for (gy = 0; gy < cmv_mb_height(m); gy++)
        for (gx = 0; gx < cmv_mb_width(m); gx++) {
            if (!cmv_static(m, gx, gy)) continue;
            for (r = gy * 16; r < gy * 16 + 16 && r < still->height; r++)
                for (c = gx * 16; c < gx * 16 + 16 && c < still->width; c++) {
                    const uint8_t *a = px + (size_t) r * stride + 4 * c;
                    const uint8_t *b = still->pixels + ((size_t) r * still->width + c) * 4;
                    for (k = 0; k < 3; k++) {
                        double av = a[k], bv = b[k];
                        sa[k] += av; sb[k] += bv; sd[k] += fabs(av - bv);
                        saa[k] += av * av; sbb[k] += bv * bv; sab[k] += av * bv;
                    }
                    n++;
                }
        }
    if (!n) { if (mad_out) *mad_out = 0; return 0; }
    for (k = 0; k < 3; k++) {
        double N = (double) n;
        double cov = sab[k] / N - (sa[k] / N) * (sb[k] / N);
        double va = saa[k] / N - (sa[k] / N) * (sa[k] / N);
        double vb = sbb[k] / N - (sb[k] / N) * (sb[k] / N);
        double cc = (va > 0 && vb > 0) ? cov / (sqrt(va) * sqrt(vb)) : 0.0;
        if (cc < worst) worst = cc;
        if (sd[k] / N > mad) mad = sd[k] / N;
    }
    if (mad_out) *mad_out = mad;
    return worst;
}

/* Opens a movie out of the archives; the caller frees *file. */
static cmv_movie *open_movie(cmvs_game *g, const char *name, uint8_t **file,
                             char *err, size_t errlen)
{
    int size = 0;
    cmv_movie *m;
    *file = cmvs_game_data(g, name, &size, err, errlen);
    if (!*file) return NULL;
    m = cmv_open(*file, size, err, errlen);
    if (!m) { free(*file); *file = NULL; }
    return m;
}

/* ---------------------------------------------- the container and the bits */

static void test_bitstream(cmvs_game *g)
{
    char err[512] = "";
    uint8_t *file = NULL;
    cmv_movie *m = open_movie(g, "bg350a.cmv", &file, err, sizeof err);
    int i;

    check(m != NULL, "bg350a.cmv is in the archives and its CMV9 container parses");
    if (!m) { printf("        %s\n", err); return; }

    printf("        %d x %d, %d frames at %d fps, step %d, macroblocks %d x %d\n",
           cmv_width(m), cmv_height(m), cmv_frames(m), cmv_fps(m), cmv_step(m),
           cmv_mb_width(m), cmv_mb_height(m));
    check(cmv_width(m) == 1280 && cmv_height(m) == 720,
          "the movie is the size of the screen");
    check(cmv_mb_width(m) * 16 >= cmv_width(m) && cmv_mb_height(m) * 16 >= cmv_height(m),
          "the macroblock grid covers the picture");
    check(cmv_frames(m) == 73 && cmv_fps(m) == 24,
          "73 frames at 24 frames a second, as the container says");
    check(cmv_audio(m, NULL, NULL) == 0,
          "a CMV9 background carries no audio track (its +0x28 is zero)");

    check(cmv_intra(m, err, sizeof err), "the intra picture decodes");
    if (err[0]) printf("        %s\n", err);
    {
        long lit_static = 0, static_blocks = 0, lit_moving = 0, moving_blocks = 0;
        const uint8_t *px = cmv_pixels(m);
        int stride = cmv_stride(m), gx, gy;
        for (gy = 0; gy < cmv_mb_height(m); gy++)
            for (gx = 0; gx < cmv_mb_width(m); gx++) {
                const uint8_t *q = px + (size_t) (gy * 16 + 8) * stride + 4 * (gx * 16 + 8);
                int lit = q[0] > 16 || q[1] > 16 || q[2] > 16;
                if (cmv_static(m, gx, gy)) { static_blocks++; lit_static += lit; }
                else { moving_blocks++; lit_moving += lit; }
            }
        printf("        %ld static macroblocks (%ld lit), %ld moving (%ld lit)\n",
               static_blocks, lit_static, moving_blocks, lit_moving);
        check(static_blocks > moving_blocks,
              "most of the picture is static, which is what makes the movie small");
        check(static_blocks > 0 && lit_static * 100 / static_blocks > 95,
              "the intra lays down the static macroblocks");
        check(moving_blocks > 0 && lit_moving * 100 / moving_blocks < 25,
              "and touches almost none of the moving ones");
    }

    check(cmv_frame(m, 0, 0, err, sizeof err), "frame 0 decodes");
    if (err[0]) printf("        %s\n", err);
    /*
     * Frame 1 is a DELTA: its format word carries 0x1000 and it brings a second
     * mask of its own, and `follows` = 1 is what lets that mask leave pixels
     * alone. Decoding it with follows = 0 must also work - that is the path
     * 0x00431760 takes when the frame is not the one after the last drawn.
     */
    check(cmv_frame(m, 1, 1, err, sizeof err), "frame 1, a delta frame, decodes");
    if (err[0]) printf("        %s\n", err);
    check(cmv_frame(m, 1, 0, err, sizeof err),
          "frame 1 decodes again with its own mask ignored, as after a seek");
    if (err[0]) printf("        %s\n", err);

    {
        int ok = 1;
        for (i = 0; i < cmv_frames(m); i++)
            if (!cmv_frame(m, i, i > 0, err, sizeof err)) { ok = 0; break; }
        check(ok, "every frame decodes in order, each consuming its own declared "
                  "coefficients and no more");
        if (!ok) printf("        frame %d: %s\n", i, err);
    }

    check(!cmv_frame(m, cmv_frames(m), 0, err, sizeof err),
          "a frame past the end is refused rather than read");
    {
        int size = 0;
        uint8_t *whole = cmvs_game_data(g, "bg350a.cmv", &size, err, sizeof err);
        if (whole) {
            check(cmv_open(whole, size / 2, err, sizeof err) == NULL,
                  "a container cut off before its frame data is refused");
            check(cmv_open(whole, 0x20, err, sizeof err) == NULL,
                  "a container shorter than its own header is refused");
            free(whole);
        }
    }

    cmv_close(m);
    free(file);
}

/* ------------------------------------------------------------- the picture */

static void test_picture(cmvs_game *g)
{
    char err[512] = "";
    uint8_t *file = NULL;
    cmv_movie *m = open_movie(g, "bg400a.cmv", &file, err, sizeof err);
    pb3_image still;
    double corr, mad;

    check(m != NULL, "bg400a.cmv, the movie that has a still of its own, opens");
    if (!m) { printf("        %s\n", err); return; }
    printf("        %d x %d, %d frames at %d fps\n",
           cmv_width(m), cmv_height(m), cmv_frames(m), cmv_fps(m));
    check(cmv_intra(m, err, sizeof err) && cmv_frame(m, 0, 0, err, sizeof err),
          "its intra and first frame decode");

    if (cmvs_game_image(g, "bg400a.pb3", &still, err, sizeof err)) {
        check(still.width == cmv_width(m) && still.height == cmv_height(m),
              "bg400a.pb3 is the same size as a frame of bg400a.cmv");
        if (still.width == cmv_width(m) && still.height == cmv_height(m)) {
            corr = still_correlation(m, &still, &mad);
            printf("        against bg400a.pb3, over the static macroblocks: "
                   "worst channel correlation %.3f (mean |difference| %.1f of 255)\n",
                   corr, mad);
            /*
             * THE TOLERANCE. 0.95 on the worst of the three channels. The two
             * are the same render of the same room, so what is left inside the
             * bound is the movie's own quantisation and nothing else; measured,
             * it sits at 0.979. A decoder that has lost its bitstream draws
             * macroblock-shaped noise and correlates below 0.2, which is what
             * two pictures of DIFFERENT rooms measure.
             */
            check(corr > 0.95,
                  "a decoded frame is the still, within 0.95 correlation per channel");
        }
        pb3_free(&still);
    } else {
        printf("  ....  no bg400a.pb3 in this game: %s\n", err);
    }
    cmv_close(m);
    free(file);

    /* And the pair that is NOT a reference, measured so that the difference
     * between "a separate render" and "a broken decoder" stays visible. */
    m = open_movie(g, "bg350a.cmv", &file, err, sizeof err);
    if (m && cmv_intra(m, err, sizeof err) && cmv_frame(m, 0, 0, err, sizeof err)
        && cmvs_game_image(g, "bg350a.pb3", &still, err, sizeof err)) {
        if (still.width == cmv_width(m) && still.height == cmv_height(m)) {
            corr = still_correlation(m, &still, &mad);
            printf("        against bg350a.pb3, a separate render of the same "
                   "carriage: %.3f (mean |difference| %.1f)\n", corr, mad);
            check(corr > 0.40,
                  "and bg350a's own still is still recognisably the same room");
        }
        pb3_free(&still);
    }
    cmv_close(m);
    free(file);
}

/* -------------------------------------------------- every movie it ships */

static void test_every_movie(cmvs_game *g)
{
    /*
     * The eight a script of this game names, and then the loose ones under
     * data/video - the opening and the endings, which are where its sound
     * tracks are.
     */
    static const char *names[] = {
        "bg350a", "bg350b", "bg400a", "bg400b", "mov03", "mov01b", "mv03", "sf01",
        "op", "mov01a", "mov04", "eda", "edb", "edc",
    };
    const int cap = 200;   /* op.cmv is three thousand frames; the first two
                            * hundred say whether its bitstream reads. */
    size_t i;
    int decoded = 0, refused = 0, broken = 0, with_audio = 0;
    for (i = 0; i < sizeof names / sizeof *names; i++) {
        char err[512] = "", cmv[64];
        uint8_t *file = NULL;
        cmv_movie *m;
        int f;
        snprintf(cmv, sizeof cmv, "%s.cmv", names[i]);
        m = open_movie(g, cmv, &file, err, sizeof err);
        if (!m) {
            printf("        %-10s refused: %s\n", cmv, err);
            refused++;
            continue;
        }
        if (!cmv_intra(m, err, sizeof err)) { broken++; printf("        %-10s intra: %s\n", cmv, err); }
        else {
            int want = cmv_frames(m) < cap ? cmv_frames(m) : cap;
            for (f = 0; f < want; f++)
                if (!cmv_frame(m, f, f > 0, err, sizeof err)) break;
            if (f < want) {
                broken++;
                printf("        %-10s frame %d of %d: %s\n", cmv, f, cmv_frames(m), err);
            } else {
                const uint8_t *track = NULL;
                int track_size = 0;
                decoded++;
                /*
                 * A movie's sound is the LAST entry of its own frame table, the
                 * one whose codec is 0, and the header's +0x28 says whether it
                 * is there (0x00432633). ChronoClock's background loops carry
                 * none and its scene movies carry an Ogg Vorbis track.
                 */
                if (cmv_audio(m, &track, &track_size)) with_audio++;
                printf("        %-10s %d x %d, %d frames at %d fps, %d decoded%s\n",
                       cmv, cmv_width(m), cmv_height(m), cmv_frames(m), cmv_fps(m),
                       want, track_size ? ", with an Ogg track" : "");
                if (track_size)
                    check(track_size > 4 && !memcmp(track, "OggS", 4),
                          "the track a movie carries is an Ogg stream");
            }
        }
        cmv_close(m);
        free(file);
    }
    printf("        %d movies decode, %d refused by name, %d broke, %d carry sound\n",
           decoded, refused, broken, with_audio);
    check(decoded >= 4, "the CMV9 movies in the archives all decode, every frame");
    check(broken == 0,
          "and no movie is half-decoded: what is not covered is refused, not guessed");
    check(with_audio > 0, "and a movie that carries a sound track is found to carry one");
}

/* ------------------------------------------------------------- the scene */

static void test_scene(const char *folder, const char *saves, int budget)
{
    char err[512] = "";
    cmvs_session *s;
    int object = -1, frame = -1, frames = 0, playing = 0;
    int found = 0, first_frame = -1, moved = 0, black = 100, loaded = 0;
    int i;

    s = cmvs_session_open(folder, NULL, NULL, saves, err, sizeof err);
    check(s != NULL, "the session opens");
    if (!s) { printf("        %s\n", err); return; }
    for (i = 0; i < 200; i++) cmvs_session_frame(s, err, sizeof err);
    loaded = saves && cmvs_session_load_slot(s, 0, err, sizeof err);
    printf("        %s\n", loaded ? "from the reference save" : "from a new game");

    /*
     * The walk stops at the movie that is 73 frames long, which is bg350a.cmv
     * and nothing else in this game: snky02.ps3 plays other movies on the way
     * and the background loop of the monorail is the one this is about.
     */
    for (i = 0; i < budget; i++) {
        if (!read_on(s, 1)) break;
        if (cmvs_session_movie(s, 0, &object, &frame, &frames, &playing) && frames == 73) {
            found = 1;
            break;
        }
    }
    printf("        %d frames read\n", i);
    check(found, "command 0x308 puts bg350a.cmv in slot 0 while reading snky02.ps3");
    if (!found) { cmvs_session_close(s); return; }

    printf("        object %d, %d frames, frame %d, %s\n",
           object, frames, frame, playing ? "playing" : "stopped");
    check(object == 28, "it draws into object 28, the background object");
    check(frame >= 0, "a frame is on the surface the moment the movie opens");

    first_frame = frame;
    for (i = 0; i < 240 && !moved; i++) {
        read_on(s, 1);
        if (!cmvs_session_movie(s, 0, &object, &frame, &frames, &playing)) break;
        if (frame != first_frame) moved = 1;
    }
    check(moved, "the frame advances on the engine's own clock");
    check(playing, "0x309 left the player running");

    /*
     * THE TRACK LOOPS. snky02.ps3 asks for 0x309(1, -1, 0, 0, 0): track 0 of
     * slot 0, first frame 0, last frame -1 which is the movie's own last, and
     * the loop flag SET. So the player must walk to frame 72, go back to frame
     * 0 (0x00431ca3) and still be running - the branch beside it, 0x00431cec,
     * is the one that would stop it. A movie background that stopped after
     * three seconds would look exactly like one that worked, for three seconds.
     */
    {
        int seen_late = 0, wrapped = 0, previous = frame;
        for (i = 0; i < 4000 && !wrapped; i++) {
            if (!idle(s, 1)) break;
            if (!cmvs_session_movie(s, 0, &object, &frame, &frames, &playing)) break;
            if (frame > frames - 8) seen_late = 1;
            if (seen_late && frame < previous) wrapped = 1;
            previous = frame;
        }
        printf("        %d frames later the movie is at frame %d of %d, %s\n",
               i, frame, frames, playing ? "playing" : "stopped");
        check(seen_late, "the movie plays through to the end of its track");
        check(wrapped, "and the track loops back to its first frame");
        check(playing, "rather than stopping there");
    }

    black = black_fraction(cmvs_session_pixels(s), cmvs_session_width(s),
                           cmvs_session_height(s));
    printf("        the frame above the message window is %d%% black\n", black);
    /*
     * With 0x308 unimplemented this stretch measured 83% black for fifty
     * message lines. Read to FROM A NEW GAME it is now nothing at all: the
     * movie fills the frame the way a still background does.
     *
     * Reached through a LOAD instead it measures about 51%, and that is not the
     * movie: after a load this engine places a staged item at about half the
     * size the original gives it, which shortens every background in the scene
     * and not only this one - the scenes either side of the monorail, which are
     * ordinary .pb3 stills, measure 27% on the same walk and nothing on a walk
     * from a new game. That is a separate defect and it is written down as one,
     * which is why the bound is the looser 60 when the walk starts from a save.
     */
    check(black < (loaded ? 60 : 10), "the monorail scene is no longer black");

    cmvs_session_close(s);
}

int main(int argc, char **argv)
{
    const char *saves = NULL, *folder = NULL;
    char err[512] = "";
    cmvs_game *g;
    int i;

    /* A folder that holds a cmvs.cfg is a game and anything else is the saves,
     * which is how test_saves tells its own two arguments apart. */
    for (i = 1; i < argc; i++) {
        char probe[1024];
        FILE *f;
        snprintf(probe, sizeof probe, "%s/cmvs.cfg", argv[i]);
        f = fopen(probe, "rb");
        if (f) { fclose(f); if (!folder) folder = argv[i]; }
        else if (!saves) saves = argv[i];
    }

    printf("cmv: the .cmv video path\n");
    if (!folder) {
        printf("  ....  no CMVS game on this machine; nothing to decode\n");
        printf("cmv: %d checks, %d failed\n", checks, failures);
        return 0;
    }
    printf("  game: %s\n", folder);

    g = cmvs_game_open(folder, err, sizeof err);
    check(g != NULL, "the game opens");
    if (g) {
        test_bitstream(g);
        test_picture(g);
        test_every_movie(g);
        cmvs_game_close(g);
    } else {
        printf("        %s\n", err);
    }

    /*
     * The scene, played to from a NEW GAME so that the check needs nothing but
     * the game itself - the same shape test_toolbar uses. A saves folder given
     * on the command line is used as a shortcut when it holds a slot, because
     * the monorail is a long way in.
     */
    test_scene(folder, saves, saves ? 6000 : 20000);

    printf("cmv: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
