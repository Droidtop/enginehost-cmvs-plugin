#include "game.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * How many archives a game may have. Which archives it HAS is read off the
 * pack folder: the engine binary names none of them, the names come from the
 * mounts the game's own boot script makes, and a game with no voices or no
 * video simply ships fewer files. A fixed list was a fact about one game.
 */
#define ARCHIVE_MAX 32

/*
 * How many loose files under the game folder are remembered for the by-name
 * search. ChronoClock's tree holds about two hundred; the bound is here so a
 * game with a huge unpacked folder degrades to "not found" rather than to
 * unbounded memory, and it is reported when it is reached.
 */
#define LOOSE_MAX 4096

struct cmvs_game {
    char folder[512];
    char pack[1024];            /* absolute, with a trailing slash; real folder only */
    char pack_rel[1024];        /* relative to the game root, with a trailing slash */
    int width, height;
    char font[128];             /* the first FONT= family, cp932 as written */
    cpz_archive *archive[ARCHIVE_MAX];
    char archive_name[ARCHIVE_MAX][64];
    int archives;
    int script_archive;         /* index of the archive that holds scripts */

    /*
     * A host file broker in place of a real folder (Enginehost
     * docs/engine-sandbox.md "Host file service design"), or NULL for an
     * ordinary in-process launch. Every open below asks this instead of
     * fopen()/opendir() when it is set; nothing about the LOOKUPS changes,
     * only how a name becomes a descriptor.
     */
    const cmvs_broker *broker;

    /* Every loose file under the game folder, indexed on the first sound the
     * scripts ask for and kept for the rest of the session. The two strings
     * are on the heap because a game's own folder names decide how long a path
     * is, and a fixed row wide enough for the deepest of them would be mostly
     * air in every other one. loose_path is always RELATIVE to the game
     * folder root, whether it came from a real recursive walk or from the
     * broker's own listing, so read_loose() has one way to open it either
     * way. */
    int loose_indexed;
    int loose;
    char *loose_name[LOOSE_MAX];
    char *loose_path[LOOSE_MAX];
};

int cmvs_game_archives(const cmvs_game *g) { return g ? g->archives : 0; }

cpz_archive *cmvs_game_archive(const cmvs_game *g, int i)
{
    return (g && i >= 0 && i < g->archives) ? g->archive[i] : NULL;
}

const char *cmvs_game_archive_name(const cmvs_game *g, int i)
{
    return (g && i >= 0 && i < g->archives) ? g->archive_name[i] : "";
}

const char *cmvs_game_pack(const cmvs_game *g) { return g ? g->pack : ""; }

const char *cmvs_game_font(const cmvs_game *g) { return g ? g->font : ""; }

static void fail(char *err, size_t errlen, const char *msg)
{
    if (err && errlen) { strncpy(err, msg, errlen - 1); err[errlen - 1] = 0; }
}

/* A file relative to the game folder's own root - "cmvs.cfg", or a loose
 * entry found under it - opened for reading either from the real folder or
 * through the broker, whichever this game was opened with. This is the one
 * place that decides which of the two a relative name means. */
static FILE *game_open_read(cmvs_game *g, const char *relpath)
{
    if (g->broker) {
        int fd = g->broker->open_read(g->broker->ctx, relpath);
        if (fd < 0) return NULL;
        return fdopen(fd, "rb");
    }
    {
        char path[2600];
        snprintf(path, sizeof path, "%s/%s", g->folder, relpath);
        return fopen(path, "rb");
    }
}

/* cmvs.cfg is a Windows INI in cp932 with backslash paths. Only two keys
 * matter here, and both live in it verbatim. */
static void read_config(cmvs_game *g)
{
    char line[512];
    FILE *f;

    g->width = 1280;
    g->height = 720;
    snprintf(g->pack_rel, sizeof g->pack_rel, "data/pack/");
    if (!g->broker) snprintf(g->pack, sizeof g->pack, "%.500s/data/pack/", g->folder);

    f = game_open_read(g, "cmvs.cfg");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        if (!strncmp(line, "SCRIPT_INIT_PATH=", 17)) {
            char *p;
            snprintf(g->pack_rel, sizeof g->pack_rel, "%.400s", line + 17);
            for (p = g->pack_rel; *p; p++) if (*p == 0x5C) *p = '/';
            if (p > g->pack_rel && p[-1] != '/') { *p++ = '/'; *p = 0; }
            if (!g->broker) snprintf(g->pack, sizeof g->pack, "%.500s/%s", g->folder, g->pack_rel);
        } else if (!strncmp(line, "FONT=", 5)) {
            if (!g->font[0]) snprintf(g->font, sizeof g->font, "%.120s", line + 5);
        } else if (!strncmp(line, "WINDOW_WIDTH=", 13)) {
            int v = atoi(line + 13);
            if (v > 0) g->width = v;
        } else if (!strncmp(line, "WINDOW_HEIGHT=", 14)) {
            int v = atoi(line + 14);
            if (v > 0) g->height = v;
        }
    }
    fclose(f);
}

static int name_order(const void *a, const void *b)
{
    return strcmp((const char *) a, (const char *) b);
}

/*
 * Every *.cpz in the pack folder, in name order. Name order is not the order
 * the boot script mounts them in, and it does not have to be: an entry is
 * addressed by the leaf of its stored path and those are unique across a
 * game's archives. What matters is that the set comes from the game.
 */
static void collect_archives(cmvs_game *g)
{
    struct dirent *e;

    if (g->broker) {
        char names[CMVS_BROKER_LIST_MAX][256];
        char listed[1024];
        int total, i;
        snprintf(listed, sizeof listed, "%s", g->pack_rel);
        /* The broker's own list() takes a plain relative path, without the
           trailing slash a real opendir() would not need either. */
        {
            size_t n = strlen(listed);
            while (n > 0 && listed[n - 1] == '/') listed[--n] = 0;
        }
        total = g->broker->list(g->broker->ctx, listed, names, CMVS_BROKER_LIST_MAX);
        for (i = 0; i < total && g->archives < ARCHIVE_MAX; i++) {
            size_t len = strlen(names[i]);
            if (len < 5 || len >= sizeof g->archive_name[0]) continue;
            if (strcasecmp(names[i] + len - 4, ".cpz")) continue;
            snprintf(g->archive_name[g->archives], sizeof g->archive_name[0], "%s", names[i]);
            g->archives++;
        }
    } else {
        DIR *d = opendir(g->pack);
        if (!d) return;
        while ((e = readdir(d)) && g->archives < ARCHIVE_MAX) {
            size_t len = strlen(e->d_name);
            if (len < 5 || len >= sizeof g->archive_name[0]) continue;
            if (strcasecmp(e->d_name + len - 4, ".cpz")) continue;
            snprintf(g->archive_name[g->archives], sizeof g->archive_name[0],
                     "%s", e->d_name);
            g->archives++;
        }
        closedir(d);
    }
    qsort(g->archive_name, (size_t) g->archives, sizeof g->archive_name[0],
          name_order);
}

/* Opens one archive already named in g->archive_name, from the real pack
 * folder or through the broker, whichever this game has. */
static cpz_archive *open_archive(cmvs_game *g, const char *name, char *why, size_t whylen)
{
    if (g->broker) {
        char relpath[2600];
        int fd;
        snprintf(relpath, sizeof relpath, "%s%s", g->pack_rel, name);
        fd = g->broker->open_read(g->broker->ctx, relpath);
        return cpz_open_fd(fd, why, whylen);
    }
    {
        char path[2600];
        snprintf(path, sizeof path, "%s%s", g->pack, name);
        return cpz_open(path, why, whylen);
    }
}

static cmvs_game *open_common(cmvs_game *g, char *err, size_t errlen)
{
    int i, opened = 0;

    g->script_archive = -1;
    read_config(g);
    collect_archives(g);
    for (i = 0; i < g->archives; i++) {
        char why[256];
        g->archive[i] = open_archive(g, g->archive_name[i], why, sizeof why);
        if (!g->archive[i]) continue;
        opened++;
        /* The scripts are wherever a PS2A container is filed, and every CMVS
         * game so far calls that archive script.cpz; the test is the content
         * of the archive rather than its name only where the name does not
         * settle it, because opening one entry per archive is not free. */
        if (g->script_archive < 0 && !strcmp(g->archive_name[i], "script.cpz"))
            g->script_archive = i;
    }
    if (!opened) {
        fail(err, errlen, "no CPZ archive could be opened under the pack folder");
        cmvs_game_close(g);
        return NULL;
    }
    return g;
}

cmvs_game *cmvs_game_open(const char *folder, char *err, size_t errlen)
{
    cmvs_game *g = calloc(1, sizeof *g);
    if (!g) { fail(err, errlen, "out of memory"); return NULL; }
    snprintf(g->folder, sizeof g->folder, "%s", folder);
    return open_common(g, err, errlen);
}

cmvs_game *cmvs_game_open_via_broker(const cmvs_broker *broker, char *err, size_t errlen)
{
    cmvs_game *g;
    if (!broker) { fail(err, errlen, "no host file broker for the game folder"); return NULL; }
    g = calloc(1, sizeof *g);
    if (!g) { fail(err, errlen, "out of memory"); return NULL; }
    g->broker = broker;
    return open_common(g, err, errlen);
}

void cmvs_game_close(cmvs_game *g)
{
    int i;
    if (!g) return;
    for (i = 0; i < g->archives; i++) if (g->archive[i]) cpz_close(g->archive[i]);
    for (i = 0; i < g->loose; i++) { free(g->loose_name[i]); free(g->loose_path[i]); }
    free(g);
}

int cmvs_game_width(const cmvs_game *g) { return g->width; }
int cmvs_game_height(const cmvs_game *g) { return g->height; }

/* strdup is not in C11 and this tree builds as -std=c11 on both machines. */
static char *dup_string(const char *s)
{
    size_t n = strlen(s) + 1;
    char *copy = malloc(n);
    if (copy) memcpy(copy, s, n);
    return copy;
}

/* `relpath` is relative to the game folder root either way, so this is the
 * one reader index_loose's table and cmvs_game_script's loose lookup both
 * call. */
static uint8_t *read_loose(cmvs_game *g, const char *relpath, int *size_out)
{
    FILE *f = game_open_read(g, relpath);
    long size;
    uint8_t *data;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
        fclose(f);
        return NULL;
    }
    data = malloc((size_t) size ? (size_t) size : 1);
    if (!data) { fclose(f); return NULL; }
    if (fread(data, 1, (size_t) size, f) != (size_t) size) {
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size_out = (int) size;
    return data;
}

int cmvs_game_script(cmvs_game *g, const char *name, cmvs_script *out,
                     char *err, size_t errlen)
{
    char relpath[2600], path[2600];
    uint8_t *raw, *data;
    int size = 0, expanded = 0;

    snprintf(relpath, sizeof relpath, "%s%s", g->pack_rel, name);
    raw = read_loose(g, relpath, &size);
    if (raw) {
        data = cmvs_unpack_ps2(raw, size, &expanded, err, errlen);
        free(raw);
        if (!data) return 0;
        return cmvs_script_open(data, expanded, out, err, errlen);
    }

    if (g->script_archive >= 0) {
        cpz_archive *a = g->archive[g->script_archive];
        const cpz_entry *e;
        snprintf(path, sizeof path, "code/%s", name);
        e = cpz_find(a, path);
        if (!e) e = cpz_find(a, name);
        if (e) {
            data = cpz_read(a, e, &size, err, errlen);
            if (!data) return 0;
            return cmvs_script_open(data, size, out, err, errlen);
        }
    }
    snprintf(path, sizeof path, "no script named %.200s", name);
    fail(err, errlen, path);
    return 0;
}

/*
 * The boot script. cmvs32.exe carries "start.ps3" and "main.ps3" as literals -
 * the name is the engine's, not the game's - and a PS2-generation build names
 * the same two files .ps2. So ask the game which generation it ships rather
 * than assuming one.
 */
const char *cmvs_game_boot_script(cmvs_game *g)
{
    static const char *const NAMES[] = { "start.ps3", "start.ps2" };
    int i;
    for (i = 0; i < 2; i++) {
        cmvs_script probe;
        char why[256];
        if (cmvs_game_script(g, NAMES[i], &probe, why, sizeof why)) {
            cmvs_script_close(&probe);
            return NAMES[i];
        }
    }
    return NAMES[0];
}

/* Where a type 6 overlay's base image is looked up: the same archive and the
 * same directory inside it, because that is how the game names them. */
typedef struct {
    cpz_archive *archive;
    char dir[1024];
} base_context;

static uint8_t *load_base(void *ctx, const char *name, int *size_out)
{
    base_context *c = ctx;
    char path[2200];
    const cpz_entry *e;
    char why[256];

    snprintf(path, sizeof path, "%s%s", c->dir, name);
    e = cpz_find(c->archive, path);
    if (!e) e = cpz_find(c->archive, name);
    if (!e) return NULL;
    return cpz_read(c->archive, e, size_out, why, sizeof why);
}

static int decode_here(cpz_archive *a, const cpz_entry *e, pb3_image *img,
                       char *err, size_t errlen)
{
    base_context ctx;
    const char *slash = strrchr(e->name, '/');
    uint8_t *data;
    int size = 0, ok;

    ctx.archive = a;
    ctx.dir[0] = 0;
    if (slash) {
        size_t n = (size_t) (slash - e->name) + 1;
        if (n >= sizeof ctx.dir) n = sizeof ctx.dir - 1;
        memcpy(ctx.dir, e->name, n);
        ctx.dir[n] = 0;
    }
    data = cpz_read(a, e, &size, err, errlen);
    if (!data) return 0;
    ok = pb3_decode(data, size, load_base, &ctx, img, err, errlen);
    free(data);
    return ok;
}

static void loose_add(cmvs_game *g, const char *leaf, const char *relpath)
{
    char *name, *full;
    if (g->loose >= LOOSE_MAX) return;
    name = dup_string(leaf);
    full = dup_string(relpath);
    if (!name || !full) { free(name); free(full); return; }
    g->loose_name[g->loose] = name;
    g->loose_path[g->loose] = full;
    g->loose++;
}

static void index_folder(cmvs_game *g, const char *path, int depth)
{
    DIR *d;
    struct dirent *e;

    if (depth > 3) return;                  /* data/<kind>/<sub>/ is as deep as it goes */
    d = opendir(path);
    if (!d) return;
    while ((e = readdir(d))) {
        char child[1600];
        struct stat st;
        if (e->d_name[0] == '.') continue;
        snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            index_folder(g, child, depth + 1);
        } else {
            /* Relative to the game folder root, the same as the broker walk
             * below stores, so read_loose() never needs to know which of the
             * two found this entry. */
            loose_add(g, e->d_name, child + strlen(g->folder) + 1);
        }
    }
    closedir(d);
}

/*
 * The same walk over the broker's own listing. A directory's list() answers
 * with its children; a plain file's answers empty, the same as an empty
 * directory would - so an empty real directory is indexed here as if it were
 * a (never matched, never opened) loose file, which costs nothing and reads
 * nothing, rather than needing the broker to say which of the two a name is.
 */
static void index_folder_broker(cmvs_game *g, const char *relpath, int depth)
{
    char names[CMVS_BROKER_LIST_MAX][256];
    int total, i;

    if (depth > 3) return;
    total = g->broker->list(g->broker->ctx, relpath, names, CMVS_BROKER_LIST_MAX);
    if (total <= 0) return;
    for (i = 0; i < total; i++) {
        char child[1600];
        char sub[CMVS_BROKER_LIST_MAX][256];
        if (relpath[0]) snprintf(child, sizeof child, "%s/%s", relpath, names[i]);
        else snprintf(child, sizeof child, "%s", names[i]);
        if (g->broker->list(g->broker->ctx, child, sub, CMVS_BROKER_LIST_MAX) > 0) {
            index_folder_broker(g, child, depth + 1);
        } else {
            loose_add(g, names[i], child);
        }
    }
}

static void index_loose(cmvs_game *g)
{
    if (g->loose_indexed) return;
    g->loose_indexed = 1;
    if (g->broker) index_folder_broker(g, "", 0);
    else index_folder(g, g->folder, 0);
}

uint8_t *cmvs_game_data(cmvs_game *g, const char *name, int *size_out,
                        char *err, size_t errlen)
{
    char why[512];
    int i;

    if (!g || !name || !name[0]) return NULL;

    index_loose(g);
    for (i = 0; i < g->loose; i++) {
        if (strcasecmp(g->loose_name[i], name)) continue;
        uint8_t *data = read_loose(g, g->loose_path[i], size_out);
        if (data) return data;
    }

    for (i = 0; i < g->archives; i++) {
        const cpz_entry *e;
        if (!g->archive[i]) continue;
        e = cpz_find(g->archive[i], name);
        if (!e) e = cpz_find_leaf(g->archive[i], name);
        if (!e) continue;
        return cpz_read(g->archive[i], e, size_out, err, errlen);
    }

    snprintf(why, sizeof why, "no file named %.200s in the game", name);
    fail(err, errlen, why);
    return NULL;
}

int cmvs_game_image(cmvs_game *g, const char *name, pb3_image *out,
                    char *err, size_t errlen)
{
    char with_suffix[1200], why[1400];
    const char *wanted = name;
    size_t len = strlen(name);
    int i;

    if (len < 4 || strcmp(name + len - 4, ".pb3")) {
        snprintf(with_suffix, sizeof with_suffix, "%s.pb3", name);
        wanted = with_suffix;
    }
    /*
     * Every archive files its entries under a directory, and the directory is
     * NOT the archive's name: chip.cpz holds "chip/title01_chip.pb3" but bg.cpz
     * holds "pb3/bg990a.pb3" and stand/up/balloon a folder per chapter. The
     * bytecode names the leaf alone, so each archive is asked for the name as
     * given and then for a leaf match, archive by archive.
     * While the lookup guessed the archive's own name as the prefix, every
     * background in the game was missing - which is why the played scene had
     * nothing behind its message window.
     */
    for (i = 0; i < g->archives; i++) {
        const cpz_entry *e;
        if (!g->archive[i]) continue;
        e = cpz_find(g->archive[i], wanted);
        if (!e) e = cpz_find_leaf(g->archive[i], wanted);
        if (!e) continue;
        if (decode_here(g->archive[i], e, out, err, errlen)) return 1;
        return 0;
    }
    snprintf(why, sizeof why, "no image named %s in any archive", wanted);
    fail(err, errlen, why);
    return 0;
}
</content>
