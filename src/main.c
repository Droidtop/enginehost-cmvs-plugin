/*
 * Desktop runner for the CMVS engine.
 *
 * This is the harness the engine is developed against: it runs on Linux with no
 * Android in the picture, so every format is proven here before the plugin
 * wraps it.
 *
 *   cmvs <game folder>                       list what the archives hold
 *   cmvs <game folder> --check               decode a sample of every image
 *   cmvs <game folder> --show <entry>        open a window on one image
 *   cmvs <game folder> --run [script] [-v]   run the bytecode from start.ps3
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "cpz.h"
#include "game.h"
#include "interp.h"
#include "pb3.h"
#include "script.h"
#include "vm.h"

static const char *ARCHIVES[] = {
    "ps.cpz", "script.cpz", "bg.cpz", "chip.cpz", "balloon.cpz",
    "stand.cpz", "up.cpz", "event.cpz", "se.cpz", "video.cpz",
};
static const int ARCHIVE_COUNT = (int) (sizeof ARCHIVES / sizeof ARCHIVES[0]);

/* Where a type 6 overlay's base image is looked up: the same archive, and the
 * same directory inside it, because that is how the game names them. */
typedef struct {
    cpz_archive *archive;
    const char *dir;   /* "" or "sub/" */
} base_context;

static uint8_t *load_base(void *ctx, const char *name, int *size_out)
{
    base_context *c = ctx;
    char path[1024];
    const cpz_entry *e;
    char err[256];

    snprintf(path, sizeof path, "%s%s", c->dir, name);
    e = cpz_find(c->archive, path);
    if (!e) e = cpz_find(c->archive, name);
    if (!e) return NULL;
    return cpz_read(c->archive, e, size_out, err, sizeof err);
}

static int decode_entry(cpz_archive *a, const cpz_entry *e, pb3_image *img,
                        char *err, size_t errlen)
{
    base_context ctx;
    char dir[1024] = "";
    const char *slash = strrchr(e->name, '/');
    uint8_t *data;
    int size = 0, ok;

    if (slash) {
        size_t n = (size_t) (slash - e->name) + 1;
        if (n >= sizeof dir) n = sizeof dir - 1;
        memcpy(dir, e->name, n);
        dir[n] = 0;
    }
    ctx.archive = a;
    ctx.dir = dir;

    data = cpz_read(a, e, &size, err, errlen);
    if (!data) return 0;
    ok = pb3_decode(data, size, load_base, &ctx, img, err, errlen);
    free(data);
    return ok;
}

static int cmd_list(const char *game)
{
    char path[4096];
    int i, total = 0;
    for (i = 0; i < ARCHIVE_COUNT; i++) {
        char err[256] = {0};
        cpz_archive *a;
        snprintf(path, sizeof path, "%s/data/pack/%s", game, ARCHIVES[i]);
        a = cpz_open(path, err, sizeof err);
        if (!a) { printf("%-12s -- %s\n", ARCHIVES[i], err); continue; }
        printf("%-12s scheme=%-14s entries=%d\n", ARCHIVES[i], cpz_scheme_name(a), cpz_count(a));
        total += cpz_count(a);
        cpz_close(a);
    }
    printf("\n%d entries readable in total\n", total);
    return 0;
}

static int cmd_check(const char *game, int per_archive)
{
    char path[4096];
    int i, decoded = 0, failed = 0;

    for (i = 0; i < ARCHIVE_COUNT; i++) {
        char err[256] = {0};
        cpz_archive *a;
        int n, images = 0, ok = 0, bad = 0, step, k;
        Uint32 t0;
        int by_type[16];

        snprintf(path, sizeof path, "%s/data/pack/%s", game, ARCHIVES[i]);
        a = cpz_open(path, err, sizeof err);
        if (!a) continue;
        n = cpz_count(a);
        for (k = 0; k < n; k++) {
            const char *name = cpz_at(a, k)->name;
            size_t len = strlen(name);
            if (len > 4 && !strcmp(name + len - 4, ".pb3")) images++;
        }
        if (!images) { cpz_close(a); continue; }

        memset(by_type, 0, sizeof by_type);
        step = images / per_archive;
        if (step < 1) step = 1;
        t0 = SDL_GetTicks();
        {
            int seen = 0;
            for (k = 0; k < n; k++) {
                const cpz_entry *e = cpz_at(a, k);
                size_t len = strlen(e->name);
                pb3_image img;
                if (len <= 4 || strcmp(e->name + len - 4, ".pb3")) continue;
                if (seen++ % step) continue;
                err[0] = 0;
                if (decode_entry(a, e, &img, err, sizeof err)) {
                    ok++;
                    pb3_free(&img);
                } else {
                    bad++;
                    if (bad <= 3) printf("      FAIL %s: %s\n", e->name, err);
                }
            }
        }
        printf("%-12s pb3=%-5d sampled=%-4d decoded=%-4d failed=%-3d  (%u ms)\n",
               ARCHIVES[i], images, ok + bad, ok, bad, (unsigned) (SDL_GetTicks() - t0));
        decoded += ok;
        failed += bad;
        cpz_close(a);
    }
    printf("\n%d images decoded, %d failed\n", decoded, failed);
    return failed ? 1 : 0;
}

/* Decodes every script and reports whether the instruction set holds up. */
static int cmd_scripts(const char *game, const char *listing)
{
    char path[4096], err[256] = {0};
    cpz_archive *a;
    int k, n, clean = 0, total = 0;
    long statements = 0, commands = 0, expressions = 0, unknown = 0, strings = 0;

    snprintf(path, sizeof path, "%s/data/pack/script.cpz", game);
    a = cpz_open(path, err, sizeof err);
    if (!a) { fprintf(stderr, "script.cpz: %s\n", err); return 1; }
    n = cpz_count(a);
    for (k = 0; k < n; k++) {
        const cpz_entry *e = cpz_at(a, k);
        cmvs_script script;
        cmvs_walk_stats st;
        uint8_t *data;
        int size = 0, ok;
        data = cpz_read(a, e, &size, err, sizeof err);
        if (!data) { printf("%-24s read: %s\n", e->name, err); continue; }
        if (!cmvs_script_open(data, size, &script, err, sizeof err)) {
            printf("%-24s %s\n", e->name, err);
            free(data);
            continue;
        }
        total++;
        ok = cmvs_walk(&script, &st);
        clean += ok;
        statements += st.statements;
        commands += st.commands;
        expressions += st.expressions;
        unknown += st.unknown;
        strings += st.strings;
        if (!ok) {
            printf("%-24s %d statements, %d unknown", e->name, st.statements, st.unknown);
            if (st.first_unknown_pc >= 0)
                printf(", first 0x%04x at %06x", st.first_unknown_op, st.first_unknown_pc);
            printf("\n");
        }
        if (listing && !strcmp(listing, e->name)) cmvs_disassemble(&script, 40, stdout);
        cmvs_script_close(&script);
    }
    cpz_close(a);
    printf("\n%d/%d scripts decoded to the exact end with no unknown opcode\n", clean, total);
    printf("%ld statements: %ld expressions, %ld commands, %ld unknown; %ld string references\n",
           statements, expressions, commands, unknown, strings);
    return clean == total ? 0 : 1;
}

static int cmd_show(const char *game, const char *wanted)
{
    char path[4096];
    char err[256] = {0};
    cpz_archive *a = NULL;
    const cpz_entry *e = NULL;
    pb3_image img;
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    int i, running = 1;

    for (i = 0; i < ARCHIVE_COUNT; i++) {
        snprintf(path, sizeof path, "%s/data/pack/%s", game, ARCHIVES[i]);
        a = cpz_open(path, err, sizeof err);
        if (!a) continue;
        e = cpz_find(a, wanted);
        if (e) break;
        cpz_close(a);
        a = NULL;
    }
    if (!a || !e) { fprintf(stderr, "No entry named %s in any archive\n", wanted); return 1; }
    if (!decode_entry(a, e, &img, err, sizeof err)) {
        fprintf(stderr, "%s: %s\n", wanted, err);
        cpz_close(a);
        return 1;
    }
    cpz_close(a);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        pb3_free(&img);
        return 1;
    }
    window = SDL_CreateWindow("CMVS", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              img.width, img.height, SDL_WINDOW_RESIZABLE);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STATIC, img.width, img.height);
    SDL_SetTextureBlendMode(texture, img.has_alpha ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
    SDL_UpdateTexture(texture, NULL, img.pixels, 4 * img.width);

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT
                || (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)) running = 0;
        }
        SDL_SetRenderDrawColor(renderer, 0x20, 0x20, 0x24, 0xFF);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    pb3_free(&img);
    return 0;
}

/* Writes BGRA out as a PNG, so a headless run can still be looked at. */
static int cmd_png(const char *game, const char *wanted, const char *out_path)
{
    char path[4096], err[256] = {0};
    cpz_archive *a = NULL;
    const cpz_entry *e = NULL;
    pb3_image img;
    SDL_Surface *surface;
    int i, rc;

    for (i = 0; i < ARCHIVE_COUNT; i++) {
        snprintf(path, sizeof path, "%s/data/pack/%s", game, ARCHIVES[i]);
        a = cpz_open(path, err, sizeof err);
        if (!a) continue;
        e = cpz_find(a, wanted);
        if (e) break;
        cpz_close(a);
        a = NULL;
    }
    if (!a || !e) { fprintf(stderr, "No entry named %s in any archive\n", wanted); return 1; }
    if (!decode_entry(a, e, &img, err, sizeof err)) {
        fprintf(stderr, "%s: %s\n", wanted, err);
        cpz_close(a);
        return 1;
    }
    cpz_close(a);

    surface = SDL_CreateRGBSurfaceWithFormatFrom(img.pixels, img.width, img.height, 32,
                                                 4 * img.width, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) { fprintf(stderr, "SDL surface: %s\n", SDL_GetError()); pb3_free(&img); return 1; }
    rc = SDL_SaveBMP(surface, out_path);
    SDL_FreeSurface(surface);
    if (rc != 0) fprintf(stderr, "SDL_SaveBMP: %s\n", SDL_GetError());
    else printf("%s -> %s (%dx%d, alpha=%s)\n", wanted, out_path, img.width, img.height,
                img.has_alpha ? "yes" : "no");
    pb3_free(&img);
    return rc != 0;
}


/* Runs the bytecode. This is the milestone the engine is built towards: the
 * boot script executing far enough that the commands it calls are the real
 * work list for what to implement next. */
static int cmd_run(const char *folder, const char *script, int trace, long budget)
{
    char err[256] = {0};
    cmvs_game *g = cmvs_game_open(folder, err, sizeof err);
    cmvs_interp *in;
    int rc, kinds = 0, missing;

    if (!g) { fprintf(stderr, "%s\n", err); return 1; }
    in = cmvs_interp_new(g);
    if (!in) { cmvs_game_close(g); return 1; }
    cmvs_interp_trace(in, trace);
    if (!cmvs_interp_boot(in, script, err, sizeof err)) {
        fprintf(stderr, "%s: %s\n", script, err);
        cmvs_interp_free(in);
        cmvs_game_close(g);
        return 1;
    }
    rc = cmvs_interp_step(in, budget, err, sizeof err);
    if (rc < 0) fprintf(stderr, "stopped: %s\n", err);
    cmvs_interp_report(in, stdout);
    missing = cmvs_interp_unimplemented(in, &kinds);
    printf("%d calls to %d commands that are not implemented yet\n", missing, kinds);
    cmvs_interp_free(in);
    cmvs_game_close(g);
    return rc < 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
            "usage: %s <game folder> [--check [n] | --show <entry> | --bmp <entry> <out.bmp>]\n",
            argv[0]);
        return 2;
    }
    if (argc >= 3 && !strcmp(argv[2], "--check")) {
        return cmd_check(argv[1], argc >= 4 ? atoi(argv[3]) : 40);
    }
    if (argc >= 3 && !strcmp(argv[2], "--scripts")) {
        return cmd_scripts(argv[1], argc >= 4 ? argv[3] : NULL);
    }
    if (argc >= 3 && !strcmp(argv[2], "--run")) {
        const char *script = "start.ps3";
        int trace = 0, i;
        long budget = 2000000;
        for (i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "-v")) trace = 1;
            else if (!strcmp(argv[i], "-vv")) trace = 2;
            else if (!strcmp(argv[i], "-n") && i + 1 < argc) budget = atol(argv[++i]);
            else script = argv[i];
        }
        return cmd_run(argv[1], script, trace, budget);
    }
    if (argc >= 4 && !strcmp(argv[2], "--show")) return cmd_show(argv[1], argv[3]);
    if (argc >= 5 && !strcmp(argv[2], "--bmp")) return cmd_png(argv[1], argv[3], argv[4]);
    return cmd_list(argv[1]);
}
