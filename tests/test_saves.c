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
    if (dir && game) test_state(dir, game);
    else printf("no game to save from; the engine round trip needs one\n");

    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
