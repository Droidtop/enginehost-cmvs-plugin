/*
 * The scheme table, tested.
 *
 * A CPZ archive carries nothing that names the scheme it was encrypted with, so
 * cpz_open tries every scheme in the generated table and keeps the one whose
 * index checksum verifies. That makes the table the one thing standing between
 * this engine and an unseen CMVS game: a row that is wrong, duplicated or
 * unreachable is not a compile error and not a crash - it is a game that
 * refuses to open, months later, on somebody elses console.
 *
 * So there are two different things to check here.
 *
 *  - The tables own shape, which needs no game at all: every row named, every
 *    row naming an MD5 variant the implementation really has, and no two rows
 *    carrying the same constants, because two identical rows mean the try-each
 *    loop cannot tell those two games apart.
 *  - Then, for every game folder named on the command line, that every archive
 *    in it opens, that they all agree on ONE scheme, that the entries are sane,
 *    and that one entry actually decrypts - an index that verifies is not yet
 *    proof that the per-entry key schedule is right.
 *
 * It ends by saying how many of the known schemes have been through real data.
 * That number is smaller than the table and is meant to be: only ChronoClock is
 * here to test with. It is printed rather than hidden so the gap stays visible
 * instead of being mistaken for coverage.
 *
 *   make test                      the tables shape, and ChronoClock if present
 *   make test GAMES="/a /b /c"     and whatever else is on this machine
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmvs_md5.h"
#include "cpz.h"
#include "game.h"
#include "scheme.h"

static int failures;

static void failed(const char *fmt, ...)
{
    va_list ap;
    fputs("  FAIL: ", stdout);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar(10);
    failures++;
}

#define REQUIRE(cond, ...) do { if (!(cond)) failed(__VA_ARGS__); } while (0)

/* Two rows are interchangeable when every constant the reader uses matches. */
static int same_constants(const cmvs_scheme *a, const cmvs_scheme *b)
{
    return a->md5_variant == b->md5_variant
        && memcmp(a->secret, b->secret, 24 * sizeof(uint32_t)) == 0
        && a->decoder_factor == b->decoder_factor
        && a->entry_init_key == b->entry_init_key
        && a->entry_sub_key == b->entry_sub_key
        && a->entry_tail_key == b->entry_tail_key
        && a->entry_key_pos == b->entry_key_pos
        && a->index_seed == b->index_seed
        && a->index_addend == b->index_addend
        && a->index_subtrahend == b->index_subtrahend
        && memcmp(a->dir_key_addend, b->dir_key_addend, sizeof a->dir_key_addend) == 0;
}

static void test_table(void)
{
    int i, j;
    printf("scheme table: %d schemes\n", cmvs_scheme_count);
    REQUIRE(cmvs_scheme_count > 0, "the scheme table is empty");

    for (i = 0; i < cmvs_scheme_count; i++) {
        const cmvs_scheme *s = &cmvs_schemes[i];
        REQUIRE(s->name && s->name[0], "scheme %d has no name", i);
        if (!s->name || !s->name[0]) continue;
        REQUIRE(s->secret != NULL, "%s has no secret table", s->name);
        REQUIRE(s->md5_variant >= 0 && s->md5_variant < CMVS_MD5_VARIANT_COUNT,
                "%s names MD5 variant %d, which does not exist",
                s->name, (int) s->md5_variant);
        REQUIRE(s->entry_key_pos >= 0 && s->entry_key_pos < 16,
                "%s has entry_key_pos %d, outside the sixteen-word key",
                s->name, s->entry_key_pos);
        for (j = 0; j < i; j++) {
            if (!cmvs_schemes[j].name) continue;
            REQUIRE(strcmp(s->name, cmvs_schemes[j].name) != 0,
                    "two schemes are both called %s", s->name);
            REQUIRE(!same_constants(s, &cmvs_schemes[j]),
                    "%s and %s carry identical constants, so the reader cannot "
                    "tell those two games apart", cmvs_schemes[j].name, s->name);
        }
    }
}

/*
 * Every MD5 variant has to be a real permutation and not the dispatch falling
 * through to a neighbour: two variants that answer the same on the same input
 * would let one game open with another games scheme and nobody would see it.
 */
static void test_md5_variants(void)
{
    static const uint32_t probe[4] = {0x01234567u, 0x89abcdefu, 0xfedcba98u, 0x76543210u};
    uint32_t out[CMVS_MD5_VARIANT_COUNT][4];
    int used[CMVS_MD5_VARIANT_COUNT];
    int i, j;

    memset(used, 0, sizeof used);
    for (i = 0; i < CMVS_MD5_VARIANT_COUNT; i++)
        cmvs_md5((cmvs_md5_variant) i, probe, out[i]);
    for (i = 0; i < cmvs_scheme_count; i++) used[cmvs_schemes[i].md5_variant] = 1;

    for (i = 0; i < CMVS_MD5_VARIANT_COUNT; i++)
        for (j = 0; j < i; j++)
            REQUIRE(memcmp(out[i], out[j], sizeof out[i]) != 0,
                    "MD5 variants %d and %d answer the same, so one of the two "
                    "is not implemented", j, i);

    for (i = 0; i < CMVS_MD5_VARIANT_COUNT; i++)
        if (!used[i])
            printf("  note: MD5 variant %d is implemented and no scheme uses it\n", i);
}

/* Marks the scheme this game turned out to need, so the summary can say how
 * much of the table real data has been through. */
static void test_game(const char *folder, int *proven, int proven_max)
{
    char err[256];
    cmvs_game *g;
    const char *scheme = NULL;
    int i, n, entries_total = 0;

    g = cmvs_game_open(folder, err, sizeof err);
    if (!g) {
        failed("%s does not open: %s", folder, err);
        return;
    }
    n = cmvs_game_archives(g);
    REQUIRE(n > 0, "%s has no archives in its pack folder", folder);

    for (i = 0; i < n; i++) {
        cpz_archive *a = cmvs_game_archive(g, i);
        const char *aname = cmvs_game_archive_name(g, i);
        const char *sname;
        int count, k, read_one = 0;

        if (!a) {
            failed("%s: no known scheme opens %s", folder, aname);
            continue;
        }
        sname = cpz_scheme_name(a);
        if (!scheme) scheme = sname;
        REQUIRE(strcmp(scheme, sname) == 0,
                "%s: %s opens as %s while the rest open as %s, so one of the "
                "two is matching an archive it does not belong to",
                folder, aname, sname, scheme);

        count = cpz_count(a);
        REQUIRE(count > 0, "%s: %s has no entries", folder, aname);
        entries_total += count;
        for (k = 0; k < count; k++) {
            const cpz_entry *e = cpz_at(a, k);
            REQUIRE(e && e->name && e->name[0],
                    "%s: %s entry %d has no name", folder, aname, k);
            if (!e || !e->name) break;
            REQUIRE(e->size >= 0 && e->offset >= 0,
                    "%s: %s entry %s has a nonsense extent", folder, aname, e->name);
        }
        for (k = 0; k < count && !read_one; k++) {
            const cpz_entry *e = cpz_at(a, k);
            int size = 0;
            uint8_t *data;
            if (!e || e->size <= 0) continue;
            data = cpz_read(a, e, &size, err, sizeof err);
            REQUIRE(data != NULL, "%s: %s: %s does not decrypt: %s",
                    folder, aname, e->name, err);
            if (data) { read_one = 1; free(data); }
        }
    }

    if (scheme)
        for (i = 0; i < cmvs_scheme_count && i < proven_max; i++)
            if (strcmp(cmvs_schemes[i].name, scheme) == 0) proven[i] = 1;

    printf("  %s: %d archives, %d entries, scheme %s\n",
           folder, n, entries_total, scheme ? scheme : "(none)");
    cmvs_game_close(g);
}

int main(int argc, char **argv)
{
    int proven[64];
    int i, games = 0, proven_count = 0;

    memset(proven, 0, sizeof proven);
    test_table();
    test_md5_variants();

    printf("games:\n");
    for (i = 1; i < argc; i++) { test_game(argv[i], proven, 64); games++; }
    if (!games) printf("  (none given)\n");

    for (i = 0; i < cmvs_scheme_count && i < 64; i++) if (proven[i]) proven_count++;
    printf("%d of %d schemes proven against real archives; the rest are "
           "transcribed from GARbros database and untested for want of a copy\n",
           proven_count, cmvs_scheme_count);

    if (failures) { printf("FAILED: %d check(s)\n", failures); return 1; }
    printf("OK\n");
    return 0;
}
