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
 * Two records are expected to differ and both are named: the thumbnail, which
 * is a picture of what OUR engine had on screen, and 0x283, which is four
 * bytes longer because we write all sixty-four global strings where the
 * original wrote sixty. Anything else differing is a real regression.
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
        if (a->tag == 0x283) continue;          /* named above */
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

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : NULL;
    const char *game = argc > 2 ? argv[2] : NULL;

    test_shapes();
    if (dir) {
        printf("reference saves in %s\n", dir);
        test_slot(dir, "save000.dat");
        test_slot(dir, "save002.dat");
        test_system(dir, "system.dat");
        test_system(dir, "system.bak");
    } else {
        printf("no reference saves given; the container tests need real files\n");
    }
    if (dir && game) {
        test_state(dir, game); test_picture(dir, game); test_advance(dir, game);
        test_choice(dir, game);
    }
    else printf("no game to save from; the engine round trip needs one\n");

    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
