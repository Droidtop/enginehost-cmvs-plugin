#include "game.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The order start.ps3 registers them in; the image lookup follows it. */
static const char *ARCHIVES[] = {
    "script.cpz", "se.cpz", "voice2.cpz", "voice.cpz", "chip.cpz", "bg.cpz",
    "event.cpz", "stand.cpz", "up.cpz", "balloon.cpz", "ps.cpz", "video.cpz",
};
#define ARCHIVE_COUNT ((int) (sizeof ARCHIVES / sizeof ARCHIVES[0]))

struct cmvs_game {
    char folder[512];
    char pack[1024];            /* absolute, with a trailing slash */
    int width, height;
    cpz_archive *archive[ARCHIVE_COUNT];
    int script_archive;         /* index of script.cpz, or -1 */
};

static void fail(char *err, size_t errlen, const char *msg)
{
    if (err && errlen) { strncpy(err, msg, errlen - 1); err[errlen - 1] = 0; }
}

/* cmvs.cfg is a Windows INI in cp932 with backslash paths. Only two keys
 * matter here, and both live in it verbatim. */
static void read_config(cmvs_game *g)
{
    char path[2400], line[512];
    FILE *f;

    g->width = 1280;
    g->height = 720;
    snprintf(g->pack, sizeof g->pack, "%.500s/data/pack/", g->folder);

    snprintf(path, sizeof path, "%s/cmvs.cfg", g->folder);
    f = fopen(path, "rb");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        if (!strncmp(line, "SCRIPT_INIT_PATH=", 17)) {
            char *p;
            snprintf(g->pack, sizeof g->pack, "%.500s/%.400s", g->folder, line + 17);
            for (p = g->pack; *p; p++) if (*p == 0x5C) *p = '/';
            if (p > g->pack && p[-1] != '/') { *p++ = '/'; *p = 0; }
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

cmvs_game *cmvs_game_open(const char *folder, char *err, size_t errlen)
{
    cmvs_game *g = calloc(1, sizeof *g);
    int i, opened = 0;

    if (!g) { fail(err, errlen, "out of memory"); return NULL; }
    snprintf(g->folder, sizeof g->folder, "%s", folder);
    g->script_archive = -1;
    read_config(g);

    for (i = 0; i < ARCHIVE_COUNT; i++) {
        char path[2600], why[256];
        snprintf(path, sizeof path, "%s%s", g->pack, ARCHIVES[i]);
        g->archive[i] = cpz_open(path, why, sizeof why);
        if (!g->archive[i]) continue;
        opened++;
        if (!strcmp(ARCHIVES[i], "script.cpz")) g->script_archive = i;
    }
    if (!opened) {
        fail(err, errlen, "no CPZ archive could be opened under the pack folder");
        cmvs_game_close(g);
        return NULL;
    }
    return g;
}

void cmvs_game_close(cmvs_game *g)
{
    int i;
    if (!g) return;
    for (i = 0; i < ARCHIVE_COUNT; i++) if (g->archive[i]) cpz_close(g->archive[i]);
    free(g);
}

int cmvs_game_width(const cmvs_game *g) { return g->width; }
int cmvs_game_height(const cmvs_game *g) { return g->height; }

static uint8_t *read_loose(const char *path, int *size_out)
{
    FILE *f = fopen(path, "rb");
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
    char path[2600];
    uint8_t *raw, *data;
    int size = 0, expanded = 0;

    snprintf(path, sizeof path, "%s%s", g->pack, name);
    raw = read_loose(path, &size);
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
    for (i = 0; i < ARCHIVE_COUNT; i++) {
        const cpz_entry *e;
        if (!g->archive[i]) continue;
        e = cpz_find(g->archive[i], wanted);
        if (!e) continue;
        if (decode_here(g->archive[i], e, out, err, errlen)) return 1;
        return 0;
    }
    snprintf(why, sizeof why, "no image named %s in any archive", wanted);
    fail(err, errlen, why);
    return 0;
}
