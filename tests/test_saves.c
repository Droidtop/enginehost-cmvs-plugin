/*
 * The save formats, against the real files.
 *
 * The fixtures are the saves the user made in ChronoClock on the Windows host
 * - two slots and a system file - and the test is the only claim that matters:
 * we read them, and what we write back is the SAME BYTES. A save of ours is a
 * PC save or it is nothing, so an approximate codec would pass no test here.
 *
 * The files are not in this repository: they are one person's saves of one
 * game and they belong beside the analysis in the coordination folder. Point
 * the test at a directory holding save000.dat, save002.dat and system.dat, or
 * run it with none and it says so and passes - the same shape as the archive
 * test, which tries whichever games this machine happens to have.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "camera.h"
#include "game.h"
#include "save.h"
#include "session.h"

static int failures;
static int checks;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("  FAIL  %s\n", what);
    } else {
        printf("  ok    %s\n", what);
    }
}

/* A scratch folder for the one file this test writes. Nothing of the game's
 * own is touched: the engine is handed this as its save base. */
static const char *game_scratch(void)
{
    const char *tmp = getenv("TMPDIR");
    return tmp && *tmp ? tmp : "/tmp";
}

static uint8_t *slurp(const char *path, int *size_out);

static uint8_t *slurp_from(const char *dir, const char *name, int *size_out)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    return slurp(path, size_out);
}

static int write_out(const char *path, const uint8_t *data, int size)
{
    FILE *f = fopen(path, "wb");
    int ok;
    if (!f) return 0;
    ok = fwrite(data, 1, (size_t) size, f) == (size_t) size;
    fclose(f);
    return ok;
}

static uint8_t *slurp(const char *path, int *size_out)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long size;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return NULL; }
    buf = malloc((size_t) size);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t) size, f) != (size_t) size) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *size_out = (int) size;
    return buf;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16)
         | ((uint32_t) p[3] << 24);
}

/* Where the first difference is, so a failure says something. */
static long first_difference(const uint8_t *a, int an, const uint8_t *b, int bn)
{
    int n = an < bn ? an : bn, i;
    for (i = 0; i < n; i++) if (a[i] != b[i]) return i;
    return an == bn ? -1 : n;
}

static void test_slot(const char *dir, const char *name)
{
    char path[1024], err[256], what[256];
    uint8_t *file, *again = NULL;
    int size = 0, again_size = 0;
    cmvs_save save;
    cmvs_record *r;

    snprintf(path, sizeof path, "%s/%s", dir, name);
    file = slurp(path, &size);
    if (!file) {
        printf("  --    %s: not on this machine\n", name);
        return;
    }
    printf("%s (%d bytes)\n", name, size);

    err[0] = 0;
    if (!cmvs_save_read(file, size, &save, err, sizeof err)) {
        snprintf(what, sizeof what, "%s reads: %s", name, err);
        check(0, what);
        free(file);
        return;
    }
    snprintf(what, sizeof what, "%s reads", name);
    check(1, what);

    /* The record list has to consume the stream exactly, which it does only if
     * every tag's shape is right - one wrong shape and the next tag is noise. */
    snprintf(what, sizeof what, "%s: %d records", name, save.records);
    check(save.records > 40, what);

    /* Fields SAVE-FORMAT.md names, so a silent change of meaning is caught. */
    r = cmvs_save_find(&save, 0x102, 0);
    check(r && r->len == 11 && strcmp((char *) r->data, "snky01.ps3") == 0,
          "0x102 is the running script, snky01.ps3");
    r = cmvs_save_find(&save, 0x243, 0);
    check(r && r->len == 65536, "0x243 is the 64 KB data stack");
    r = cmvs_save_find(&save, 0x280, 0);
    check(r && r->len == 8192, "0x280 is int globals 0..2047");
    r = cmvs_save_find(&save, 0x281, 0);
    check(r && r->len == 4096, "0x281 is float globals 0..1023");
    r = cmvs_save_find(&save, 0x282, 0);
    check(r && r->len == 256, "0x282 is flag bytes 0..255");
    r = cmvs_save_find(&save, 0x10b, 0);
    check(r && r->len == 1796, "0x10b is the 64 registered procedures");
    r = cmvs_save_find(&save, 0x242, 0);
    check(r && r->len == 256, "0x242 is the 64 frame pointers");

    /* The thumbnail is a plain 24-bit BMP, 192 x 108, bottom-up. */
    check(save.thumb && save.thumb_size == 54 + 192 * 108 * 3
          && save.thumb[0] == 'B' && save.thumb[1] == 'M'
          && rd32(save.thumb + 18) == 192,
          "the thumbnail is a 192x108 BMP");
    /* Stream C is the script the resume point is in, carried whole. */
    check(save.script && save.script_size > 0
          && memcmp(save.script, "PS2A", 4) == 0,
          "the script image is a PS2A container");

    /* The claim this test exists for. */
    err[0] = 0;
    again = cmvs_save_write(&save, &again_size, err, sizeof err);
    if (!again) {
        snprintf(what, sizeof what, "%s rewrites: %s", name, err);
        check(0, what);
    } else {
        long at = first_difference(file, size, again, again_size);
        if (at >= 0)
            printf("        first difference at 0x%lx (%d bytes vs %d)\n",
                   at, size, again_size);
        snprintf(what, sizeof what, "%s is rewritten BYTE FOR BYTE", name);
        check(at < 0, what);
    }

    free(again);
    cmvs_save_free(&save);
    free(file);
}

static void test_system(const char *dir, const char *name)
{
    char path[1024], err[256], what[256];
    uint8_t *file, *again = NULL;
    int size = 0, again_size = 0;
    cmvs_system sys;

    snprintf(path, sizeof path, "%s/%s", dir, name);
    file = slurp(path, &size);
    if (!file) {
        printf("  --    %s: not on this machine\n", name);
        return;
    }
    printf("%s (%d bytes)\n", name, size);

    err[0] = 0;
    if (!cmvs_system_read(file, size, &sys, err, sizeof err)) {
        snprintf(what, sizeof what, "%s reads: %s", name, err);
        check(0, what);
        free(file);
        return;
    }
    check(1, "system.dat reads");
    check(sys.size >= CMVS_SYS_STRINGS_OFF,
          "the payload holds the persistent halves of every global array");

    /*
     * Where the user's short session actually left its mark, which is also
     * what fixes the block boundaries: 238 non-zero bytes in the PERSISTENT
     * INT GLOBALS, every one of them the low byte of a dword, and exactly one
     * non-zero byte in the whole 32512-byte persistent flag half. Those ints
     * are what the System Configuration screen writes - CMVS has no settings
     * struct, the config screen is a script writing globals.
     *
     * SAVE-FORMAT.md reads those 238 as a read-text bitmap in the FLAG block.
     * They are not in the flag block: it holds one non-zero byte. That claim
     * is wrong and this is the check that says so.
     */
    {
        int i, flagged = 0, ints = 0;
        for (i = 0; i < CMVS_SYS_FLAGS_SIZE; i++)
            if (sys.payload[CMVS_SYS_FLAGS_OFF + i]) flagged++;
        for (i = 0; i < CMVS_SYS_INTS_SIZE; i++)
            if (sys.payload[CMVS_SYS_INTS_OFF + i]) ints++;
        printf("        %d non-zero flag bytes, %d non-zero bytes of persistent ints\n",
               flagged, ints);
        check(ints > 100, "the persistent int globals carry the settings that were set");
    }

    err[0] = 0;
    again = cmvs_system_write(&sys, &again_size, err, sizeof err);
    if (!again) {
        snprintf(what, sizeof what, "%s rewrites: %s", name, err);
        check(0, what);
    } else {
        long at = first_difference(file, size, again, again_size);
        if (at >= 0)
            printf("        first difference at 0x%lx (%d bytes vs %d)\n",
                   at, size, again_size);
        check(at < 0, "system.dat is rewritten BYTE FOR BYTE");
    }

    /* Written fresh, with cipher parameters of our own, it still reads back. */
    free(again);
    cmvs_system_params(&sys, 0x51ee);
    err[0] = 0;
    again = cmvs_system_write(&sys, &again_size, err, sizeof err);
    if (!again) {
        check(0, "a system file written with our own cipher parameters");
    } else {
        cmvs_system back;
        err[0] = 0;
        if (!cmvs_system_read(again, again_size, &back, err, sizeof err)) {
            printf("        %s\n", err);
            check(0, "our own system file reads back");
        } else {
            check(back.size == sys.size
                  && memcmp(back.payload, sys.payload, (size_t) sys.size) == 0,
                  "our own system file reads back to the same payload");
            cmvs_system_free(&back);
        }
    }

    free(again);
    cmvs_system_free(&sys);
    free(file);
}

/*
 * A record list built from nothing, so the writer is proved without a fixture
 * to lean on: it must lay out all four record shapes and read back the same.
 */
static void test_shapes(void)
{
    cmvs_save s;
    uint8_t big[600];
    uint8_t music[6];
    uint8_t *file;
    int size = 0, i;
    cmvs_save save;
    char err[256];

    printf("a save built from nothing\n");
    for (i = 0; i < (int) sizeof big; i++) big[i] = (uint8_t) (i * 7);
    /* 0x304: u16 len; name; u16 len; name */
    music[0] = 1; music[1] = 0; music[2] = 'a';   /* u16 length, then the name */
    music[3] = 1; music[4] = 0; music[5] = 'b';

    cmvs_save_init(&s);
    memcpy(s.header, "CSV2", 4);
    snprintf((char *) s.header + CMVS_SAVE_CAPTION, 0x100, "2026-01-02 03:04:05 Test");
    cmvs_save_set(&s, 0x107, 0, "\x11\x22\x33\x44", 4);   /* short */
    cmvs_save_set(&s, 0x243, 0, big, (int) sizeof big);   /* long */
    cmvs_save_set(&s, 0x400, 7, big, 128);                /* indexed */
    cmvs_save_set(&s, 0x304, 0, music, (int) sizeof music); /* the music */
    cmvs_save_stamp(&s, 2026, 9, 11, 22, 33, 44);

    err[0] = 0;
    file = cmvs_save_write(&s, &size, err, sizeof err);
    if (!file) {
        printf("        %s\n", err);
        check(0, "a save built from nothing writes");
        cmvs_save_free(&s);
        return;
    }
    check(1, "a save built from nothing writes");
    err[0] = 0;
    if (!cmvs_save_read(file, size, &save, err, sizeof err)) {
        printf("        %s\n", err);
        check(0, "and reads back");
    } else {
        cmvs_record *r = cmvs_save_find(&save, 0x400, 7);
        check(save.records == 4, "and reads back all four shapes");
        check(r && r->len == 128 && memcmp(r->data, big, 128) == 0,
              "an indexed record keeps its index and its bytes");
        r = cmvs_save_find(&save, 0x304, 0);
        check(r && r->len == (int) sizeof music
              && memcmp(r->data, music, sizeof music) == 0,
              "the music record keeps both names");
        check(memcmp(save.header + CMVS_SAVE_CAPTION,
                     "2026-09-11 22:33:44 Test", 24) == 0,
              "the timestamp is stamped over the digits and nothing else");
        cmvs_save_free(&save);
    }
    free(file);
    cmvs_save_free(&s);
}


/*
 * The engine's half: our state, written by our writer, against the file the
 * original wrote from the same point in the game.
 *
 * This is the claim the container test cannot make. It boots the game the way
 * the console does, hands it the user's save000.dat, loads it through our
 * loader and saves it again through our writer with nothing in between - so
 * every record must come back with the same length and the same bytes it went
 * in with, whether this engine models it (the pc, the stack, the globals, the
 * flags, the timers, the procedures) or carries it (the layers, the scene
 * parts, the backlog, the music).
 *
 * ONE record is expected to differ and it is named: the thumbnail, which is a
 * picture of what OUR engine had on screen. Anything else differing is a real
 * regression - including 0x283, the sixty-four global strings, which used to
 * be excused here as "four bytes longer than the original's sixty" and was in
 * fact the whole of the load defects: there is no header dword in front of
 * those strings, so reading one slid every string four slots down.
 */
static void test_state(const char *dir, const char *game)
{
    char err[256] = {0}, path[1024], saves[1024];
    cmvs_session *s;
    uint8_t *file, *reference;
    int size = 0, reference_size = 0, i, differences = 0;
    cmvs_save ours, theirs;

    printf("a save of our own, from %s\n", game);
    snprintf(saves, sizeof saves, "%s/cmvs-save-test", game_scratch());
    s = cmvs_session_open(game, NULL, NULL, saves, err, sizeof err);
    if (!s) { printf("  --    %s\n", err); return; }

    /* Enough frames for the boot script to reach command 0x016, which is what
     * names the folder the saves live in. */
    for (i = 0; i < 400 && !cmvs_session_save_folder(s); i++)
        cmvs_session_frame(s, err, sizeof err);
    if (!cmvs_session_save_folder(s)) {
        check(0, "the boot script names a save folder");
        cmvs_session_close(s);
        return;
    }
    check(1, "the boot script names a save folder");

    snprintf(path, sizeof path, "%s/save000.dat", cmvs_session_save_folder(s));
    reference = slurp_from(dir, "save000.dat", &reference_size);
    if (!reference) {
        printf("  --    no save000.dat to load\n");
        cmvs_session_close(s);
        return;
    }
    if (!write_out(path, reference, reference_size)) {
        check(0, "the reference save can be put in the save folder");
        free(reference);
        cmvs_session_close(s);
        return;
    }
    check(cmvs_session_load_slot(s, 0, err, sizeof err), "the engine loads the user's save000.dat");
    check(cmvs_session_save_slot(s, 1, err, sizeof err), "and writes slot 1 back out");

    snprintf(path, sizeof path, "%s/save001.dat", cmvs_session_save_folder(s));
    file = slurp(path, &size);
    cmvs_session_close(s);
    if (!file) { check(0, "slot 1 is on disk"); free(reference); return; }

    if (!cmvs_save_read(reference, reference_size, &theirs, err, sizeof err)
        || !cmvs_save_read(file, size, &ours, err, sizeof err)) {
        printf("  %s\n", err);
        check(0, "both saves read");
        free(file); free(reference);
        return;
    }
    check(ours.records == theirs.records, "our save carries every record theirs does");
    check(ours.script_size == theirs.script_size
          && memcmp(ours.script, theirs.script, (size_t) ours.script_size) == 0,
          "the script image travels through unchanged");
    for (i = 0; i < theirs.records; i++) {
        cmvs_record *a = &theirs.rec[i];
        cmvs_record *b = cmvs_save_find(&ours, a->tag, a->index);
        if (!b) { printf("        0x%03x missing\n", a->tag); differences++; continue; }
        if (b->len != a->len || memcmp(a->data, b->data, (size_t) a->len) != 0) {
            printf("        0x%03x/%d differs (%d bytes vs %d)\n",
                   a->tag, a->index, a->len, b->len);
            differences++;
        }
    }
    check(differences == 0, "every other record comes back byte for byte");

    cmvs_save_free(&ours);
    cmvs_save_free(&theirs);
    free(file);
    free(reference);
}

/*
 * A save of ours from a DIFFERENT point in the story.
 *
 * The round trip above proves our writer's bytes; it cannot prove that a save
 * of ours carries a state the game has never been in, because it re-saves the
 * state it just read. This does: it loads the user's save000.dat, reads on
 * through the scene the way a player does - one confirm per frame, which is
 * deterministic where a timed reveal is not - and saves where it stops.
 *
 * The stopping place is named by its LINE, not by its pc: after seven answered
 * message waits the engine is resting in the eighth line's wait, and the line
 * on screen is the text this prints. That text is what a person can check on
 * the Windows game after loading the same file.
 *
 * The thumbnail is the frame the engine actually has at that moment, so it is
 * also the check that the picture is a scene and not a blank: a save whose
 * thumbnail is one flat colour is the bug the Windows LOAD screen showed as an
 * empty slot.
 */
#define ADVANCE_LINES 7

static int distinct_colours(const uint8_t *bmp, int size)
{
    /* A coarse count: the low five bits of each channel, so a gradient does
     * not read as thousands of colours and a flat fill cannot read as more
     * than one. */
    static uint8_t seen[1 << 15];
    int i, n = 0;
    memset(seen, 0, sizeof seen);
    for (i = 54; i + 3 <= size; i += 3) {
        unsigned k = (unsigned) ((bmp[i] >> 3) << 10 | (bmp[i + 1] >> 3) << 5
                                 | (bmp[i + 2] >> 3));
        if (!seen[k]) { seen[k] = 1; n++; }
    }
    return n;
}

static void test_advance(const char *dir, const char *game)
{
    char err[256] = {0}, path[1024], saves[1024], line[4096];
    cmvs_session *s;
    uint8_t *reference, *file;
    int reference_size = 0, size = 0, i, colours;
    cmvs_save ours, theirs;
    cmvs_record *a, *b;

    printf("a save of our own from further on, in %s\n", game);
    snprintf(saves, sizeof saves, "%s/cmvs-save-test", game_scratch());
    s = cmvs_session_open(game, NULL, NULL, saves, err, sizeof err);
    if (!s) { printf("  --    %s\n", err); return; }

    for (i = 0; i < 400 && !cmvs_session_save_folder(s); i++)
        cmvs_session_frame(s, err, sizeof err);
    if (!cmvs_session_save_folder(s)) { cmvs_session_close(s); return; }

    reference = slurp_from(dir, "save000.dat", &reference_size);
    if (!reference) { cmvs_session_close(s); return; }
    snprintf(path, sizeof path, "%s/save000.dat", cmvs_session_save_folder(s));
    if (!write_out(path, reference, reference_size)) {
        free(reference); cmvs_session_close(s); return;
    }
    if (!cmvs_session_load_slot(s, 0, err, sizeof err)) {
        check(0, "the engine loads the user's save000.dat");
        free(reference); cmvs_session_close(s); return;
    }

    /*
     * A few frames with nobody pressing, so the wait the save resumes into
     * runs and says which layer the line is on; before that the engine has no
     * line yet. It takes the same confirm the rest of the reading uses: the
     * script runs its own per-frame poll loop before it reaches the line's
     * wait, and that loop only lets go on a press. One press reveals the line
     * without answering its wait, so the count is still at zero here - which
     * is what the seven-line count below is measured from.
     */
    line[0] = 0;
    for (i = 0; i < 8 && !line[0]; i++) {
        cmvs_session_frame(s, err, sizeof err);
        cmvs_session_button(s, 0, 1);
        cmvs_session_button(s, 0, 0);
        cmvs_session_message(s, line, sizeof line);
    }
    check(strstr(line, "standing on the rooftop") != NULL,
          "the loaded save is resting on the line the user saved on");

    /*
     * One confirm per frame. A press finishes a still-revealing line and a
     * press on a finished line ends its wait (command 0x153), so two frames
     * carry one line however fast the reveal is set - the run lands in the
     * same place every time, which a run that waited on the typewriter would
     * not.
     */
    for (i = 0; i < 400 && cmvs_session_messages(s) < ADVANCE_LINES; i++) {
        cmvs_session_frame(s, err, sizeof err);
        cmvs_session_button(s, 0, 1);
        cmvs_session_button(s, 0, 0);
    }
    check(cmvs_session_messages(s) == ADVANCE_LINES,
          "seven message lines are read past");

    /* A few frames with nobody pressing anything, so the line that is now on
     * screen is composed whole before its picture is taken. */
    for (i = 0; i < 5; i++) cmvs_session_frame(s, err, sizeof err);

    cmvs_session_message(s, line, sizeof line);
    printf("        the line it stopped on: %s\n", line);
    check(line[0] != 0, "and there is a line on screen to name the place by");

    check(cmvs_session_save_slot(s, 3, err, sizeof err),
          "the engine writes slot 3 (save003.dat, the LOAD screen's slot 4)");
    snprintf(path, sizeof path, "%s/save003.dat", cmvs_session_save_folder(s));
    printf("        %s\n", path);
    cmvs_session_close(s);

    file = slurp(path, &size);
    if (!file) { check(0, "save003.dat is on disk"); free(reference); return; }
    if (!cmvs_save_read(file, size, &ours, err, sizeof err)
        || !cmvs_save_read(reference, reference_size, &theirs, err, sizeof err)) {
        printf("  %s\n", err);
        check(0, "the save reads back");
        free(file); free(reference); return;
    }

    check(ours.thumb && ours.thumb_size == 54 + 192 * 108 * 3
          && ours.thumb[0] == 'B' && ours.thumb[1] == 'M',
          "it carries a 192x108 BMP thumbnail");
    colours = ours.thumb ? distinct_colours(ours.thumb, ours.thumb_size) : 0;
    printf("        the thumbnail holds %d colours\n", colours);
    check(colours > 64, "and the thumbnail is a picture, not a flat fill");

    a = cmvs_save_find(&theirs, 0x107, 0);
    b = cmvs_save_find(&ours, 0x107, 0);
    check(a && b && a->len == 4 && b->len == 4
          && memcmp(a->data, b->data, 4) != 0,
          "the resume point is NOT where the user's save000.dat resumes");

    cmvs_save_free(&ours);
    cmvs_save_free(&theirs);
    free(file);
    free(reference);
}

/*
 * THE PICTURE A LOAD PUTS BACK.
 *
 * A slot save carries the whole scene as the engine's own records, and until
 * these were applied a load restored the script position and left whatever was
 * on screen standing: the frame after loading save000.dat was the boot screen's
 * logo plate with a message window over it, and so was the thumbnail the next
 * save wrote. What the original does instead is re-create every object from its
 * record (0x0045DA5E -> 0x00435710 for a 0x700, 0x0045D46B -> 0x00451B80 for a
 * 0x400), which is what cmvs_scene_restore_object and _restore_layer are.
 *
 * The check does not need a screenshot to be honest about it. save000.dat's
 * record 0x700/29 names bg990a.pb3 - the summer sky the prologue is played in -
 * at (0, 0), 1280x720, draw order 32, and the game's own archive is where that
 * image comes from. So: decode bg990a.pb3 out of the game, bucket its colours,
 * and ask how much of the composed frame lands in those buckets. The scene
 * covers nearly all of it; the logo plate, which is the frame this same run has
 * BEFORE the load, covers very little. Both numbers are printed, so a
 * regression says which way it went rather than only that it went.
 */
#define PICTURE_BUCKETS (1 << 15)

static unsigned bucket_of(const uint8_t *bgra)
{
    return (unsigned) ((bgra[0] >> 3) << 10 | (bgra[1] >> 3) << 5 | (bgra[2] >> 3));
}

/* What share of the frame, in tenths of a percent, lands in a colour the
 * reference image also has. */
static int coverage(const uint8_t *frame, int w, int h, const uint8_t *seen)
{
    long total = (long) w * h, in = 0, i;
    if (total <= 0) return 0;
    for (i = 0; i < total; i++) if (seen[bucket_of(frame + 4 * i)]) in++;
    return (int) (in * 1000 / total);
}

static void test_picture(const char *dir, const char *game)
{
    char err[256] = {0}, path[1024], saves[1024];
    cmvs_session *s;
    cmvs_game *g;
    pb3_image sky;
    uint8_t *reference, *seen, *before = NULL;
    int reference_size = 0, i, w, h, was = 0, now = 0;
    const uint8_t *frame;

    printf("the picture a load puts back, in %s\n", game);

    g = cmvs_game_open(game, err, sizeof err);
    if (!g) { printf("  --    %s\n", err); return; }
    if (!cmvs_game_image(g, "bg990a.pb3", &sky, err, sizeof err)) {
        printf("  --    bg990a.pb3: %s\n", err);
        cmvs_game_close(g);
        return;
    }
    seen = calloc(PICTURE_BUCKETS, 1);
    if (!seen) { pb3_free(&sky); cmvs_game_close(g); return; }
    for (i = 0; i < sky.width * sky.height; i++) seen[bucket_of(sky.pixels + 4 * i)] = 1;
    pb3_free(&sky);
    cmvs_game_close(g);

    snprintf(saves, sizeof saves, "%s/cmvs-picture-test", game_scratch());
    s = cmvs_session_open(game, NULL, NULL, saves, err, sizeof err);
    if (!s) { printf("  --    %s\n", err); free(seen); return; }
    for (i = 0; i < 400 && !cmvs_session_save_folder(s); i++)
        cmvs_session_frame(s, err, sizeof err);
    if (!cmvs_session_save_folder(s)) { cmvs_session_close(s); free(seen); return; }

    w = cmvs_session_width(s);
    h = cmvs_session_height(s);
    frame = cmvs_session_pixels(s);
    if (frame && w > 0 && h > 0) {
        before = malloc((size_t) w * h * 4);
        if (before) memcpy(before, frame, (size_t) w * h * 4);
        was = coverage(frame, w, h, seen);
    }

    reference = slurp_from(dir, "save000.dat", &reference_size);
    if (!reference) { cmvs_session_close(s); free(seen); free(before); return; }
    snprintf(path, sizeof path, "%s/save000.dat", cmvs_session_save_folder(s));
    if (!write_out(path, reference, reference_size)) {
        free(reference); cmvs_session_close(s); free(seen); free(before); return;
    }
    free(reference);

    if (!cmvs_session_load_slot(s, 0, err, sizeof err)) {
        check(0, "the engine loads the user's save000.dat");
        cmvs_session_close(s); free(seen); free(before); return;
    }
    cmvs_session_frame(s, err, sizeof err);
    frame = cmvs_session_pixels(s);
    if (!frame) { check(0, "there is a composed frame after the load"); goto done; }
    now = coverage(frame, w, h, seen);

    printf("        the frame is %d.%d%% the scene's colours after the load,"
           " and was %d.%d%% before it\n", now / 10, now % 10, was / 10, was % 10);
    /*
     * 85.6% as this is written. The rest of the frame is the two display
     * layers the same save restores over the sky - the toolbar along the top
     * out of iconwindow.pb3 and the message window out of message01.pb3 - so
     * the whole screen never reads as the background alone. The logo plate
     * this replaced scored 0.0%, so the gap between a restored scene and an
     * unrestored one is the whole range and not a few points.
     */
    check(now >= 800, "the frame after the load is the prologue scene");
    check(was < 500, "and the frame before it was not (it is the boot screen)");
    check(before && memcmp(before, frame, (size_t) w * h * 4) != 0,
          "so the load changed what is on screen");
    check(cmvs_session_drawn(s) > 0, "and the compositor drew the restored items");

done:
    cmvs_session_close(s);
    free(seen);
    free(before);
}

/*
 * THE FIRST CHOICE, AND TAKING IT.
 *
 * test_advance reads seven lines; this reads the whole prologue. It is the one
 * claim the per-frame poll commands are worth making: before them the engine
 * stopped at the EIGHTH line of the prologue, in the frame loop snky01.ps3
 * runs between that line and the next scene, because the loop's questions had
 * no answers and sys[0] kept whatever the last measurement had left in it.
 * With them answered the same deterministic reading - one confirm per frame -
 * carries the story through snky01 into snky07 and stands at ChronoClock's
 * first choice.
 *
 * The choice is one option, built by menu 0 at 0x030400..0x030F8A of
 * snky07.ps3: the sheet select_chip.pb3 into graphic object 60, one item whose
 * hit rectangle is 1000 x 64 at (160, 140), and the caption
 *
 *     Would not turn back time
 *
 * which the script writes with command 0x0f8 at 0x030C16 and 0x10b at
 * 0x030CB2. re/saves/README.md carries it so a person can check the same words
 * in the Windows game.
 *
 * How the choice is FOUND is the same thing a reader sees: the lines stop
 * coming. Nothing is hard-coded about where it is - the run reads until two
 * hundred frames pass with no new line, which is the choice waiting for a
 * pointer, then puts the pointer in the middle of that hit rectangle and
 * presses. If the click is taken the lines start again, and that is the check.
 */
#define CHOICE_IDLE   200      /* frames with no new line: the choice is up */
#define CHOICE_BUDGET 12000    /* frames; it stands at the choice near 6100 */
#define CHOICE_X      660      /* the middle of the item's 1000 x 64 box */
#define CHOICE_Y      172
#define CHOICE_BOX_X  160      /* the hit rectangle itself, command 0x213 */
#define CHOICE_BOX_Y  140
#define CHOICE_BOX_W  1000
#define CHOICE_BOX_H  64
/*
 * The caption's own pen, which command 0x10c sets to (448, 178) before 0x10b
 * writes the words, and the white command 0x107 gives it. The bar art has
 * white of its own along its top edge and none at all from the pen row down,
 * so ink there is the CAPTION and nothing else - which is the whole difference
 * between a choice a reader can read and the empty bar this drew before.
 */
#define CHOICE_PEN_Y  178
#define CHOICE_INK    0xFFFFFFu

/* How many pixels of the caption's colour are under the pen row, inside the
 * item's hit rectangle. */
static int caption_ink(const cmvs_session *s)
{
    const uint8_t *px = cmvs_session_pixels(s);
    int w = cmvs_session_width(s), h = cmvs_session_height(s);
    int x, y, n = 0;
    if (!px) return 0;
    for (y = CHOICE_PEN_Y; y < CHOICE_BOX_Y + CHOICE_BOX_H && y < h; y++) {
        for (x = CHOICE_BOX_X; x < CHOICE_BOX_X + CHOICE_BOX_W && x < w; x++) {
            const uint8_t *p = px + 4 * ((size_t) y * w + x);
            uint32_t c = (uint32_t) p[2] | ((uint32_t) p[1] << 8) | ((uint32_t) p[0] << 16);
            if (c == CHOICE_INK) n++;
        }
    }
    return n;
}

static void test_choice(const char *dir, const char *game)
{
    char err[256] = {0}, path[1024], saves[1024];
    cmvs_session *s;
    uint8_t *reference;
    int reference_size = 0, i, at = 0;
    long last = -1, idle = 0, before = 0;
    int ink = 0;

    printf("the first choice, in %s\n", game);
    snprintf(saves, sizeof saves, "%s/cmvs-choice-test", game_scratch());
    s = cmvs_session_open(game, NULL, NULL, saves, err, sizeof err);
    if (!s) { printf("  --    %s\n", err); return; }
    for (i = 0; i < 400 && !cmvs_session_save_folder(s); i++)
        cmvs_session_frame(s, err, sizeof err);
    if (!cmvs_session_save_folder(s)) { cmvs_session_close(s); return; }

    reference = slurp_from(dir, "save000.dat", &reference_size);
    if (!reference) { cmvs_session_close(s); return; }
    snprintf(path, sizeof path, "%s/save000.dat", cmvs_session_save_folder(s));
    if (!write_out(path, reference, reference_size)) {
        free(reference); cmvs_session_close(s); return;
    }
    free(reference);
    if (!cmvs_session_load_slot(s, 0, err, sizeof err)) {
        check(0, "the engine loads the user's save000.dat");
        cmvs_session_close(s); return;
    }

    for (i = 0; i < CHOICE_BUDGET; i++) {
        cmvs_session_frame(s, err, sizeof err);
        cmvs_session_button(s, 0, 1);
        cmvs_session_button(s, 0, 0);
        if (cmvs_session_messages(s) != last) { last = cmvs_session_messages(s); idle = 0; continue; }
        if (++idle <= CHOICE_IDLE) continue;
        if (!at) {
            at = i;
            before = last;
            ink = caption_ink(s);
            printf("        %d pixels of caption ink inside the item's hit box\n", ink);
            printf("        the lines stop after %ld of them, at frame %d, in %s\n",
                   before, at, cmvs_session_script(s));
        }
        /* The pointer goes where the item is, and the same press that turned
         * every other line now lands on it. */
        cmvs_session_pointer(s, CHOICE_X, CHOICE_Y);
        if (cmvs_session_messages(s) > before) break;
        if (i > at + 600) break;
    }

    check(at != 0, "the run reaches a place where the lines stop: the choice");
    check(before > 1000, "and it is the whole prologue away from where it used to stop");
    printf("        after the press: %ld lines, in %s\n",
           cmvs_session_messages(s), cmvs_session_script(s));
    check(cmvs_session_messages(s) > before,
          "the press on the choice is taken and the story goes on");
    check(ink > 0, "the caption is DRAWN in the bar, not only written into it");
    cmvs_session_close(s);
}

/*
 * THE 0x380 RECORDS ARE APPLIED, NOT ONLY CARRIED.
 *
 * Each one is a text object of the table at +0xbb0 - 0x0045D2D4 makes it and
 * 0x00451950 reads the payload into it - and in these two saves they are the
 * message window (id 7, which layer 0's record registers) and the name plate
 * (id 10, layer 7). The test reads the record out of the file itself, with no
 * expected numbers written down here, and asks the loaded engine for the same
 * object: a load that carried the record instead of applying it answers with
 * the defaults 0x00451EE0 writes and fails every line below.
 */
static void test_text_records(const char *dir, const char *game, const char *name)
{
    char err[256] = {0}, path[1024], saves[1024], what[256];
    cmvs_session *sess;
    uint8_t *file;
    cmvs_save save;
    int size = 0, i, applied = 0;

    printf("the 0x380 text records of %s, in %s\n", name, game);
    file = slurp_from(dir, name, &size);
    if (!file) { printf("  --    %s: not on this machine\n", name); return; }
    err[0] = 0;
    if (!cmvs_save_read(file, size, &save, err, sizeof err)) {
        free(file); check(0, "the reference save reads"); return;
    }
    free(file);

    snprintf(saves, sizeof saves, "%s/cmvs-text-test", game_scratch());
    sess = cmvs_session_open(game, NULL, NULL, saves, err, sizeof err);
    if (!sess) { printf("  --    %s\n", err); cmvs_save_free(&save); return; }
    for (i = 0; i < 400 && !cmvs_session_save_folder(sess); i++)
        cmvs_session_frame(sess, err, sizeof err);
    if (!cmvs_session_save_folder(sess)) {
        cmvs_session_close(sess); cmvs_save_free(&save); return;
    }
    snprintf(path, sizeof path, "%s/save000.dat", cmvs_session_save_folder(sess));
    file = slurp_from(dir, name, &size);
    if (!file || !write_out(path, file, size)) {
        free(file); cmvs_session_close(sess); cmvs_save_free(&save); return;
    }
    free(file);
    if (!cmvs_session_load_slot(sess, 0, err, sizeof err)) {
        check(0, "the engine loads the reference save");
        cmvs_session_close(sess); cmvs_save_free(&save); return;
    }

    for (i = 0; i < 12; i++) {
        const cmvs_record *r = cmvs_save_find(&save, 0x380, i);
        const cmvs_text *t;
        const uint8_t *d;
        int at, w[39], j, ok;
        if (!r || r->len <= 0) continue;
        d = r->data;
        at = 6;
        while (at < r->len && d[at]) at++;
        at++;
        if (((unsigned) d[0] | ((unsigned) d[1] << 8)) >= 2) at += 8;
        if (at + 39 * 4 > r->len) { check(0, "the record parses"); continue; }
        for (j = 0; j < 39; j++) w[j] = (int) rd32(d + at + j * 4);

        applied++;
        t = cmvs_session_text(sess, i);
        snprintf(what, sizeof what, "%s: text object %d exists after the load", name, i);
        check(t != NULL, what);
        if (!t) continue;
        printf("        id %d: at (%d, %d), box %d x %d at (%d, %d), size %d, colours %06X/%06X\n",
               i, w[0], w[1], w[4], w[5], w[2], w[3], w[6],
               (unsigned) w[27] & 0xFFFFFFu, (unsigned) w[28] & 0xFFFFFFu);
        ok = t->x == w[0] && t->y == w[1]
          && t->rx == w[2] && t->ry == w[3] && t->rw == w[4] && t->rh == w[5];
        snprintf(what, sizeof what, "%s: object %d sits and is boxed where the record says", name, i);
        check(ok, what);
        ok = t->size == w[6] && t->gap == w[7] && t->lead == w[8]
          && t->kinsoku == (w[26] != 0);
        snprintf(what, sizeof what, "%s: object %d has the record's metrics", name, i);
        check(ok, what);
        ok = t->colour == (uint32_t) w[27] && t->colour2 == (uint32_t) w[28]
          && t->edge == (uint32_t) w[30];
        snprintf(what, sizeof what, "%s: object %d has the record's colours", name, i);
        check(ok, what);
        ok = t->mode == w[32] && t->fade == w[33] && t->speed == w[34]
          && t->visible == (w[35] != 0)
          && t->pen_x == w[37] && t->pen_y == w[38];
        snprintf(what, sizeof what, "%s: object %d has the record's reveal and pen", name, i);
        check(ok, what);
    }
    snprintf(what, sizeof what, "%s carries text records at all", name);
    check(applied > 0, what);
    cmvs_session_close(sess);
    cmvs_save_free(&save);
}

/* ------------------------------------------------------- the LOAD screen */

/*
 * What the list screen reads out of a slot, which is what command 0x2bd
 * (0x0046c0e0) answers. This is the container half and needs no game: the
 * caption, the timestamp packed the way 0x0047f550 packs it, and the
 * thumbnail expanded out of stream B.
 *
 * It matters that this reader is SHALLOWER than a load's. 0x0046c0e0 checks
 * the magic and nothing else, so a slot a newer engine wrote still appears in
 * the list; the last checks here are that a file whose checksum has been
 * destroyed - which cmvs_save_read refuses - still peeks, and that a fourth
 * magic byte outside the range does not.
 */
static void test_peek(const char *dir, const char *name)
{
    uint8_t *file;
    int size = 0;
    cmvs_save peek;
    char err[256] = {0}, what[256];
    const char *caption;
    unsigned when;

    file = slurp_from(dir, name, &size);
    if (!file) return;
    snprintf(what, sizeof what, "%s: the list screen's reader takes it", name);
    check(cmvs_save_peek(file, size, &peek, err, sizeof err), what);
    if (!peek.header[0]) { free(file); return; }

    caption = (const char *) peek.header + CMVS_SAVE_CAPTION;
    printf("        caption \"%s\"\n", caption);
    snprintf(what, sizeof what, "%s: the caption is a timestamp and a title", name);
    check(strlen(caption) > 19 && caption[4] == '-' && caption[7] == '-'
          && caption[10] == ' ' && caption[13] == ':' && caption[16] == ':', what);

    when = (unsigned) cmvs_save_packed_time(&peek);
    printf("        packed time %08X = %04d/%02d/%02d %02d:%02d\n", when,
           2000 + (int) ((when >> 25) & 0x7F), (int) ((when >> 21) & 0xF),
           (int) ((when >> 16) & 0x1F), (int) ((when >> 11) & 0x1F),
           (int) ((when >> 5) & 0x3F));
    snprintf(what, sizeof what, "%s: the packed time is the caption's own digits", name);
    check((int) ((when >> 25) & 0x7F) == (caption[2] - '0') * 10 + (caption[3] - '0')
          && (int) ((when >> 21) & 0xF) == (caption[5] - '0') * 10 + (caption[6] - '0')
          && (int) ((when >> 16) & 0x1F) == (caption[8] - '0') * 10 + (caption[9] - '0')
          && (int) ((when >> 11) & 0x1F) == (caption[11] - '0') * 10 + (caption[12] - '0')
          && (int) ((when >> 5) & 0x3F) == (caption[14] - '0') * 10 + (caption[15] - '0'),
          what);
    snprintf(what, sizeof what, "%s: the packed time is not zero, so a row can print it", name);
    check(when != 0, what);

    snprintf(what, sizeof what, "%s: the thumbnail comes out of stream B", name);
    check(peek.thumb != NULL && peek.thumb_size > 54, what);
    if (peek.thumb && peek.thumb_size > 0x36) {
        int w = (int) ((unsigned) peek.thumb[0x12] | ((unsigned) peek.thumb[0x13] << 8));
        int h = (int) ((unsigned) peek.thumb[0x16] | ((unsigned) peek.thumb[0x17] << 8));
        int bpp = peek.thumb[0x1c] | (peek.thumb[0x1d] << 8);
        printf("        thumbnail %dx%d, %d bpp, %d bytes\n", w, h, bpp, peek.thumb_size);
        snprintf(what, sizeof what, "%s: and it is the 192x108 24-bit BMP the reader wants", name);
        check(peek.thumb[0] == 'B' && peek.thumb[1] == 'M'
              && w == 192 && h == 108 && bpp == 24, what);
    }
    cmvs_save_free(&peek);

    {
        cmvs_save shallow;
        file[size - 1] ^= 0xFF;
        snprintf(what, sizeof what, "%s: a broken checksum still LISTS, as in the original", name);
        check(cmvs_save_peek(file, size, &shallow, err, sizeof err), what);
        cmvs_save_free(&shallow);
        file[size - 1] ^= 0xFF;
        file[3] = 'Z';
        snprintf(what, sizeof what, "%s: a fourth magic byte outside 2..9 does not", name);
        check(!cmvs_save_peek(file, size, &shallow, err, sizeof err), what);
    }
    free(file);
}

/*
 * The camera, with snky02.ps3's own numbers. The scene is the rooftop pool
 * about 1200 lines into the prologue: 0x058 gives camera 0 a reference depth
 * of 70 with 128 x 72 world units across it, 0x059 a 1280 x 720 screen, and
 * 0x062 stands the camera at (-63, 36, 25) while the background sits at
 * (0, 36, 70) with a plane of 70 and its anchor on its own centre (910, 512).
 *
 * The projection's scale is the perspective factor SQUARED (0x00443eda), and
 * that is the whole of the difference between a background that covers the
 * frame and the one that drew in its right 42 per cent with black behind
 * everything else. The check is geometric rather than numeric: the bitmap,
 * placed by its anchor at the point the camera gives, must reach past both
 * edges of the 1280-wide frame.
 */
static void test_projection(void)
{
    cmvs_camera c;
    cmvs_placement p;
    cmvs_projection at;
    float left, right, factor;

    printf("the camera, with snky02.ps3's rooftop numbers\n");
    cmvs_camera_init(&c);
    c.kind = 3;
    c.x = -63.0f; c.y = 36.0f; c.z = 25.0f;
    c.reference_depth = 70.0f; c.view_width = 128.0f; c.view_height = 72.0f;
    c.screen_width = 1280.0f; c.screen_height = 720.0f;
    c.centre_x = 640.0f; c.centre_y = 360.0f;
    c.lift = 6.0f; c.aspect_x = 1.0f; c.aspect_y = 1.0f;

    memset(&p, 0, sizeof p);
    p.x = 0.0f; p.y = 36.0f; p.z = 70.0f;
    p.plane = 70.0f;
    p.scale_x = 1.0f; p.scale_y = 1.0f;

    check(cmvs_camera_project(&c, &p, &at) != 0, "the background projects at all");
    factor = 70.0f / (70.0f - 25.0f);
    printf("        anchor at x %.1f, scale %.3f (the factor is %.3f)\n",
           at.x, at.scale_x, factor);
    check(at.scale_x > factor * factor - 0.001f && at.scale_x < factor * factor + 0.001f,
          "the scale is the perspective factor squared, not once");
    left = at.x - 910.0f * at.scale_x;
    right = left + 1820.0f * at.scale_x;
    printf("        the bitmap covers x %.1f .. %.1f\n", left, right);
    check(left <= 0.0f && right >= 1280.0f, "and so it still covers the whole frame");

    p.z = 70.0f;
    c.z = 0.0f;
    check(cmvs_camera_project(&c, &p, &at) != 0, "an item at its own depth projects");
    check(at.scale_x > 0.999f && at.scale_x < 1.001f,
          "and squaring the factor changes nothing there");
}

/*
 * THE LOAD SCREEN, THROUGH THE GAME'S OWN UI, which is the only way the
 * loaded state can be checked without a rig.
 *
 * It presses what a player presses: LOAD on the title, then slot 001, then
 * YES on the panel that comes up. Every one of those is a menu item and the
 * engine reports which one it answered, so the walk is checked at each step
 * rather than only at the end.
 *
 * Two things it proves that nothing else does. The slot tile must carry the
 * save's own picture: it is compared against the tile beside it, which has no
 * save, and before command 0x2bd the two were identical because every row drew
 * empty. And the panel must be pressable at all: before commands 0x126 and
 * 0x21b it was built at (-300, -80) with no items, so the walk stopped there.
 */
#define LOAD_ITEM        3        /* the title's LOAD, menu 0 */
#define SLOT1_ITEM      70        /* the first row of the grid, menu 1 */
#define CONFIRM_YES      1        /* the panel, menu 2 */
#define SLOT1_X         44        /* 0x213's rectangle for item 70 */
#define SLOT1_Y        199
#define SLOT2_X        244        /* item 71, and no save behind it */
#define TILE_W         192
#define TILE_H         108
#define YES_X          530        /* the middle of item 1's 140 x 64 at (460, 336) */
#define YES_Y          368

static void tile_mean(const cmvs_session *s, int x, int y, int w, int h, int out[3])
{
    const uint8_t *px = cmvs_session_pixels(s);
    int r, c, n = 0, width = cmvs_session_width(s), height = cmvs_session_height(s);
    long sum[3] = {0, 0, 0};
    out[0] = out[1] = out[2] = -1;
    if (!px) return;
    for (r = y; r < y + h && r < height; r++)
        for (c = x; c < x + w && c < width; c++) {
            const uint8_t *q = px + 4 * ((size_t) r * width + c);
            sum[0] += q[0]; sum[1] += q[1]; sum[2] += q[2];
            n++;
        }
    if (!n) return;
    out[0] = (int) (sum[0] / n);
    out[1] = (int) (sum[1] / n);
    out[2] = (int) (sum[2] / n);
}

/*
 * Points at (x, y) and presses there once every forty frames until the engine
 * answers with that item, or the frames run out. The repeat is what makes the
 * walk deterministic without hard-coding how long a screen takes to build.
 */
static int press_until(cmvs_session *s, int x, int y, int item, int cap)
{
    char err[256] = {0};
    int f, last = -1, before, now;
    before = cmvs_session_menu_events(s, &last);
    for (f = 0; f < cap; f++) {
        cmvs_session_pointer(s, x, y);
        if (f % 40 == 0) cmvs_session_button(s, 0, 1);
        if (f % 40 == 2) cmvs_session_button(s, 0, 0);
        if (cmvs_session_frame(s, err, sizeof err) <= 0) return 0;
        now = cmvs_session_menu_events(s, &last);
        if (now > before && last == item) return 1;
    }
    return 0;
}

/*
 * The message window's LAID-OUT line, one row per line of the box: the glyphs
 * the engine placed, in the places it placed them. A wrap that breaks inside a
 * word, a plate grown to a few thousand pixels, or a line drawn one word to a
 * row all show here and nowhere in the message TEXT, because
 * cmvs_session_message concatenates the glyphs and loses the breaks.
 */
static void layout_of(const cmvs_session *s, int id, char *out, size_t outlen)
{
    const cmvs_text *t = cmvs_session_text(s, id);
    size_t at = 0;
    int i, last = -100000;

    out[0] = 0;
    if (!t) return;
    at += (size_t) snprintf(out + at, outlen - at, "box %d x %d at (%d, %d) size %d:",
                            t->rw, t->rh, t->rx, t->ry, t->size);
    for (i = 0; i < t->glyphs && at + 8 < outlen; i++) {
        unsigned c = t->glyph[i].code;
        if (t->glyph[i].y != last) {
            at += (size_t) snprintf(out + at, outlen - at, "\n  %4d | ", t->glyph[i].y);
            last = t->glyph[i].y;
        }
        if (c < 0x80) out[at++] = (char) c;
        else at += (size_t) snprintf(out + at, outlen - at, "<%04x>", c);
        out[at] = 0;
    }
}

/*
 * `attract` walks the same load AFTER the title's attract sequence
 * (cdemo.ps3) has started, which is the state the rig found mangled the first
 * frame of a load. Both paths must lay the first line out identically; that
 * comparison is the fixture, not a hard-coded string.
 */
static void test_load_screen_at(const char *dir, const char *game, int attract,
                                char *layout, size_t layoutlen)
{
    char err[256] = {0}, path[1024], saves[1024];
    cmvs_session *s;
    uint8_t *reference;
    int reference_size = 0, i;
    int full[3], empty[3], apart;
    const cmvs_text *t;

    printf("the Data Load screen%s, in %s\n", attract ? " after the attract" : "", game);
    snprintf(saves, sizeof saves, "%s/cmvs-loadscreen-test%d", game_scratch(), attract);
    s = cmvs_session_open(game, NULL, NULL, saves, err, sizeof err);
    if (!s) { printf("  --    %s\n", err); return; }
    for (i = 0; i < 400 && !cmvs_session_save_folder(s); i++)
        cmvs_session_frame(s, err, sizeof err);
    if (!cmvs_session_save_folder(s)) { cmvs_session_close(s); return; }

    reference = slurp_from(dir, "save000.dat", &reference_size);
    if (!reference) { cmvs_session_close(s); return; }
    snprintf(path, sizeof path, "%s/save000.dat", cmvs_session_save_folder(s));
    if (!write_out(path, reference, reference_size)) {
        free(reference); cmvs_session_close(s); return;
    }
    free(reference);
    /* The row beside it has to be genuinely empty for the comparison below. */
    snprintf(path, sizeof path, "%s/save001.dat", cmvs_session_save_folder(s));
    remove(path);

    if (attract) {
        /* The title starts cdemo.ps3 on its own after about fifty seconds of
         * being left alone; this is that wait. */
        for (i = 0; i < 8000; i++) {
            if (cmvs_session_frame(s, err, sizeof err) <= 0) break;
            if (!strcmp(cmvs_session_script(s), "cdemo.ps3")) break;
        }
        check(!strcmp(cmvs_session_script(s), "cdemo.ps3"),
              "the title starts its attract sequence when it is left alone");
    }

    if (!press_until(s, 640, 470, LOAD_ITEM, 2000)) {
        check(0, "LOAD on the title screen answers");
        cmvs_session_close(s); return;
    }
    check(1, "LOAD on the title screen answers");
    for (i = 0; i < 200; i++) cmvs_session_frame(s, err, sizeof err);

    tile_mean(s, SLOT1_X, SLOT1_Y, TILE_W, TILE_H, full);
    tile_mean(s, SLOT2_X, SLOT1_Y, TILE_W, TILE_H, empty);
    printf("        slot 001 mean BGR %02X%02X%02X, slot 002 %02X%02X%02X\n",
           full[0], full[1], full[2], empty[0], empty[1], empty[2]);
    apart = abs(full[0] - empty[0]) + abs(full[1] - empty[1]) + abs(full[2] - empty[2]);
    check(apart > 60, "the occupied row draws its own picture, the empty row does not");

    if (!press_until(s, SLOT1_X + TILE_W / 2, SLOT1_Y + TILE_H / 2, SLOT1_ITEM, 1200)) {
        check(0, "pressing slot 001 answers with its item");
        cmvs_session_close(s); return;
    }
    check(1, "pressing slot 001 answers with its item");
    for (i = 0; i < 200; i++) cmvs_session_frame(s, err, sizeof err);

    if (!press_until(s, YES_X, YES_Y, CONFIRM_YES, 1200)) {
        check(0, "the confirmation panel is where a player can press it");
        cmvs_session_close(s); return;
    }
    check(1, "the confirmation panel is where a player can press it");
    /*
     * The load is taken the moment the panel answers YES: the pc belongs to
     * the save from then on, so the run leaves menu.ps3 and intproc.ps3 for
     * the script the save was taken in. That, and not a line count, is what
     * says the load happened - a freshly restored state has read no lines yet.
     */
    for (i = 0; i < 400; i++) {
        const char *now = cmvs_session_script(s);
        if (now && strcmp(now, "menu.ps3") && strcmp(now, "intproc.ps3")
            && strcmp(now, "intcode.ps3") && strcmp(now, "start.ps3")
            && strcmp(now, "cdemo.ps3")) break;
        cmvs_session_frame(s, err, sizeof err);
    }
    printf("        after the load: %ld lines, in %s",
           cmvs_session_messages(s), cmvs_session_script(s)), putchar(10);
    check(cmvs_session_script(s) && !strcmp(cmvs_session_script(s), "snky01.ps3"),
          "the load through the UI lands in the script the save was taken in");

    /*
     * And the loaded state itself, which is BRIEF check 2 and could only be
     * asked on a device before this walk existed. Text object 7 is the message
     * window - command 0x15d registers layer 0 under that id - and the save's
     * own 0x380 record is what it must be carrying.
     */
    t = cmvs_session_text(s, 7);
    check(t != NULL, "the message window exists after a load through the UI");
    if (t) {
        printf("        id 7: at (%d, %d), box %d x %d at (%d, %d), size %d, colours %06X/%06X\n",
               t->x, t->y, t->rw, t->rh, t->rx, t->ry, t->size,
               (unsigned) t->colour & 0xFFFFFFu, (unsigned) t->colour2 & 0xFFFFFFu);
        check(t->rw > 0 && t->rh > 0 && t->size > 0,
              "and it is boxed and sized rather than left at the defaults");
        /*
         * The plate the script sizes from its OWN wrap count. With the global
         * strings shifted the script wrapped nothing and asked for a box
         * thousands of pixels tall; four lines of 30-pixel text is 180.
         */
        check(t->rh > 0 && t->rh <= 720,
              "and its box is a plausible height rather than thousands of pixels");
    }
    if (layout) layout_of(s, 7, layout, layoutlen);
    cmvs_session_close(s);
}

/*
 * The same message line, reached by READING to it from a new game rather than
 * by loading a save. One confirm a frame, stopping on the line whose text
 * contains `needle`; the layout that comes back is what a player who never
 * saved would see, and it is the reference a load has to match.
 */
static void story_layout(const char *game, const char *needle,
                         char *out, size_t outlen)
{
    char err[256] = {0}, line[1024], saves[1024];
    cmvs_session *s;
    int f;

    out[0] = 0;
    snprintf(saves, sizeof saves, "%s/cmvs-story-layout", game_scratch());
    s = cmvs_session_open(game, NULL, NULL, saves, err, sizeof err);
    if (!s) return;
    for (f = 0; f < 4000; f++) {
        cmvs_session_pointer(s, 640, 402);
        cmvs_session_button(s, 0, 1);
        cmvs_session_button(s, 0, 0);
        if (cmvs_session_frame(s, err, sizeof err) <= 0) break;
        if (cmvs_session_message(s, line, sizeof line) && strstr(line, needle)) {
            layout_of(s, 7, out, outlen);
            break;
        }
    }
    cmvs_session_close(s);
}

/*
 * THE TWO WAYS INTO A LOAD. The rig found that a load taken after the title's
 * attract sequence had started drew its first frame mangled - one word to a
 * line, literal `n` glyphs, a plate spanning the screen - while the same slot
 * taken straight from the title drew correctly. Both are walked here through
 * the game's own menus and the laid-out line is compared, so the two cannot
 * drift apart again without a failure.
 */
static void test_load_screen(const char *dir, const char *game)
{
    char clean[4096], after[4096], story[4096];
    const char *p = NULL;
    int broken = 0;

    test_load_screen_at(dir, game, 0, clean, sizeof clean);
    test_load_screen_at(dir, game, 1, after, sizeof after);
    printf("        clean path: %s\n", clean);
    printf("        after the attract: %s\n", after);
    check(clean[0] != 0, "the clean path lays the first line out");
    check(strcmp(clean, after) == 0,
          "and a load after the attract lays it out identically");

    /*
     * And the wrap itself, against the one authority there is: the same line
     * reached by PLAYING to it. snky01.ps3 wraps its own lines - at spaces,
     * in bytecode, dropping the space it breaks on - and hands the engine a
     * string whose newlines are already in it, so the engine's width-only
     * fallback should never fire. A row that ends mid-word ("closest to th" /
     * "e sky") is that fallback firing, and it fires when the script's wrap
     * loop was fed the wrong global string. Comparing the two paths says so
     * without hard-coding a single line of English.
     */
    story_layout(game, "standing on the rooftop", story, sizeof story);
    printf("        read to it instead: %s\n", story);
    check(story[0] != 0, "the same line can be reached by reading from a new game");
    check(strcmp(clean, story) == 0,
          "and a load lays that line out exactly as reading to it does");
    (void) p; (void) broken;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : NULL;
    const char *game = argc > 2 ? argv[2] : NULL;

    test_shapes();
    test_projection();
    if (dir) {
        printf("reference saves in %s\n", dir);
        test_slot(dir, "save000.dat");
        test_slot(dir, "save002.dat");
        test_peek(dir, "save000.dat");
        test_peek(dir, "save002.dat");
        test_system(dir, "system.dat");
        test_system(dir, "system.bak");
    } else {
        printf("no reference saves given; the container tests need real files\n");
    }
    if (dir && game) {
        test_state(dir, game); test_picture(dir, game); test_advance(dir, game);
        test_text_records(dir, game, "save000.dat");
        test_text_records(dir, game, "save002.dat");
        test_choice(dir, game);
        test_load_screen(dir, game);
    }
    else printf("no game to save from; the engine round trip needs one\n");

    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
