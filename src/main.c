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
 *   cmvs <game folder> --run [script] [-v] [-f frames] [--shot out.png]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "cpz.h"
#include "game.h"
#include "pb3.h"
#include "png.h"
#include "session.h"
#include "script.h"
#include "vm.h"

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
    char err[256] = {0};
    cmvs_game *g = cmvs_game_open(game, err, sizeof err);
    int i, total = 0;

    if (!g) { fprintf(stderr, "%s: %s\n", game, err); return 1; }
    for (i = 0; i < cmvs_game_archives(g); i++) {
        cpz_archive *a = cmvs_game_archive(g, i);
        const char *name = cmvs_game_archive_name(g, i);
        if (!a) { printf("%-12s -- unreadable\n", name); continue; }
        printf("%-12s scheme=%-14s entries=%d\n", name, cpz_scheme_name(a), cpz_count(a));
        total += cpz_count(a);
    }
    printf("\n%d entries readable in total\n", total);
    cmvs_game_close(g);
    return 0;
}

static int cmd_check(const char *game, int per_archive)
{
    char open_err[256] = {0};
    cmvs_game *g = cmvs_game_open(game, open_err, sizeof open_err);
    int i, decoded = 0, failed = 0;

    if (!g) { fprintf(stderr, "%s: %s\n", game, open_err); return 1; }
    for (i = 0; i < cmvs_game_archives(g); i++) {
        char err[256] = {0};
        cpz_archive *a = cmvs_game_archive(g, i);
        int n, images = 0, ok = 0, bad = 0, step, k;
        Uint32 t0;
        int by_type[16];

        if (!a) continue;
        n = cpz_count(a);
        for (k = 0; k < n; k++) {
            const char *name = cpz_at(a, k)->name;
            size_t len = strlen(name);
            if (len > 4 && !strcmp(name + len - 4, ".pb3")) images++;
        }
        if (!images) continue;

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
               cmvs_game_archive_name(g, i), images, ok + bad, ok, bad,
               (unsigned) (SDL_GetTicks() - t0));
        decoded += ok;
        failed += bad;
    }
    printf("\n%d images decoded, %d failed\n", decoded, failed);
    cmvs_game_close(g);
    return failed ? 1 : 0;
}

/* Decodes every script and reports whether the instruction set holds up. */
static int cmd_scripts(const char *game, const char *listing)
{
    char path[4096], err[256] = {0};
    cmvs_game *probe = cmvs_game_open(game, err, sizeof err);
    cpz_archive *a = NULL;
    int k, n, clean = 0, total = 0;
    long statements = 0, commands = 0, expressions = 0, unknown = 0, strings = 0;

    if (probe) {
        snprintf(path, sizeof path, "%sscript.cpz", cmvs_game_pack(probe));
        cmvs_game_close(probe);
        a = cpz_open(path, err, sizeof err);
    }
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
    char err[256] = {0};
    cmvs_game *g;
    pb3_image img;
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    int running = 1;

    /* The same lookup the running engine uses, so what this shows is what a
     * scene would get - one mechanism, not a second one that searches
     * differently. */
    g = cmvs_game_open(game, err, sizeof err);
    if (!g) { fprintf(stderr, "%s: %s\n", game, err); return 1; }
    if (!cmvs_game_image(g, wanted, &img, err, sizeof err)) {
        fprintf(stderr, "%s: %s\n", wanted, err);
        cmvs_game_close(g);
        return 1;
    }
    cmvs_game_close(g);

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
    char err[256] = {0};
    cmvs_game *g;
    pb3_image img;
    SDL_Surface *surface;
    int rc;

    g = cmvs_game_open(game, err, sizeof err);
    if (!g) { fprintf(stderr, "%s: %s\n", game, err); return 1; }
    if (!cmvs_game_image(g, wanted, &img, err, sizeof err)) {
        fprintf(stderr, "%s: %s\n", wanted, err);
        cmvs_game_close(g);
        return 1;
    }
    cmvs_game_close(g);

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


/*
 * Runs the bytecode. Everything here goes through one session, which is also
 * what the Android wrapper drives, so there is one way to start a game and the
 * desktop is not a second engine.
 *
 * There are two ways to drive it and they end in the same place. --window
 * opens an SDL2 window and feeds the mouse and the arrow keys in as the
 * pointer and the menu directions: that is the game, played. Without it the
 * run is headless, --shot writes the last frame, and --point and --click stand
 * in for a hand - they put the pointer somewhere and press it on a numbered
 * frame - so a menu can be proven in a container with no display at all.
 * --click may be given more than once, because reading a scene takes more than
 * one tap: one to leave the title and one for each line after that. --hold
 * says how many frames a click stays down; a finger on a real screen holds a
 * tap for a tenth of a second, six or seven frames, and a scene that reads
 * correctly with a one-frame press can still lose a line to a held one.
 */
#define CLICKS 16
typedef struct {
    const char *script;
    int trace;
    const char *font;
    long budget;
    int frames;
    const char *shot;
    int window;
    int point_x, point_y, has_point;
    int click_frame[CLICKS];
    int clicks;
    int hold;
} run_options;

/*
 * The window shows the game's own screen scaled and centred, which is the same
 * letterbox the Android view draws; this is its inverse, and it is why the
 * engine is handed engine coordinates and never window ones.
 */
static void to_engine(int win_w, int win_h, int w, int h, int mx, int my, int *ex, int *ey)
{
    float scale = win_w / (float) w;
    float other = win_h / (float) h;
    if (other < scale) scale = other;
    if (scale <= 0) scale = 1;
    *ex = (int) ((mx - (win_w - w * scale) / 2) / scale);
    *ey = (int) ((my - (win_h - h * scale) / 2) / scale);
}

static int report(cmvs_session *s, const run_options *o, int rc, const char *err)
{
    int kinds = 0, missing;
    if (rc < 0) fprintf(stderr, "stopped: %s\n", err);
    if (o->shot) {
        const uint8_t *pixels = cmvs_session_pixels(s);
        int w = cmvs_session_width(s), h = cmvs_session_height(s);
        char why[256] = {0};
        if (pixels && cmvs_png_write(o->shot, pixels, w, h, why, sizeof why))
            printf("%d items drawn into %s (%dx%d)\n", cmvs_session_drawn(s), o->shot, w, h);
        else
            fprintf(stderr, "%s: %s\n", o->shot, why);
    }
    cmvs_session_report(s, stdout);
    missing = cmvs_session_unimplemented(s, &kinds);
    printf("%d calls to %d commands that are not implemented yet\n", missing, kinds);
    return rc < 0;
}

/* Headless: no window, no events, and the only input is the one the flags
 * describe. This is the path --shot has always taken and it stays that way. */
static int run_headless(cmvs_session *s, const run_options *o)
{
    char err[256] = {0};
    int rc = 1, frame, i;

    for (frame = 0; frame < o->frames && rc > 0; frame++) {
        if (o->has_point) cmvs_session_pointer(s, o->point_x, o->point_y);
        /* Press on the named frame and let go `hold` frames later: a menu
         * reports a click on the release, having seen the press, so the two
         * cannot be the same frame. */
        for (i = 0; i < o->clicks; i++) {
            if (frame == o->click_frame[i]) cmvs_session_button(s, 0, 1);
            if (frame == o->click_frame[i] + o->hold) cmvs_session_button(s, 0, 0);
        }
        rc = cmvs_session_frame(s, err, sizeof err);
        if (o->trace) fprintf(stderr, "-- frame %d ends in %s\n", frame, cmvs_session_script(s));
    }
    return report(s, o, rc, err);
}

static int run_window(cmvs_session *s, const run_options *o)
{
    char err[256] = {0};
    int w = cmvs_session_width(s), h = cmvs_session_height(s);
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    int rc = 1, frame = 0, alive = 1;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    window = SDL_CreateWindow("CMVS", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              w, h, SDL_WINDOW_RESIZABLE);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!window || !renderer || !texture) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    while (alive) {
        SDL_Event ev;
        int win_w = w, win_h = h, ex, ey;
        SDL_GetWindowSize(window, &win_w, &win_h);
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                alive = 0;
                break;
            case SDL_MOUSEMOTION:
                to_engine(win_w, win_h, w, h, ev.motion.x, ev.motion.y, &ex, &ey);
                cmvs_session_pointer(s, ex, ey);
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                to_engine(win_w, win_h, w, h, ev.button.x, ev.button.y, &ex, &ey);
                cmvs_session_pointer(s, ex, ey);
                if (ev.button.button == SDL_BUTTON_LEFT || ev.button.button == SDL_BUTTON_RIGHT)
                    cmvs_session_button(s, ev.button.button == SDL_BUTTON_LEFT ? 0 : 1,
                                        ev.type == SDL_MOUSEBUTTONDOWN);
                break;
            case SDL_KEYDOWN:
                /* The pad half of a menu: the poll moves its selection on a
                 * direction and warps the pointer onto it. */
                if (ev.key.keysym.sym == SDLK_UP) cmvs_session_navigate(s, -1);
                else if (ev.key.keysym.sym == SDLK_DOWN) cmvs_session_navigate(s, 1);
                else if (ev.key.keysym.sym == SDLK_RETURN) cmvs_session_button(s, 0, 1);
                else if (ev.key.keysym.sym == SDLK_ESCAPE) alive = 0;
                break;
            case SDL_KEYUP:
                if (ev.key.keysym.sym == SDLK_RETURN) cmvs_session_button(s, 0, 0);
                break;
            default:
                break;
            }
        }
        if (!alive) break;
        if (rc > 0) {
            rc = cmvs_session_frame(s, err, sizeof err);
            if (o->trace) fprintf(stderr, "-- frame %d ends in %s\n", frame, cmvs_session_script(s));
            frame++;
            if (o->frames > 0 && frame >= o->frames) alive = 0;
        }
        SDL_UpdateTexture(texture, NULL, cmvs_session_pixels(s), 4 * w);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0xFF);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return report(s, o, rc, err);
}

static int cmd_run(const char *folder, const run_options *o)
{
    char err[256] = {0};
    cmvs_session *s = cmvs_session_open(folder, o->script, o->font, err, sizeof err);
    int rc;

    if (!s) { fprintf(stderr, "%s\n", err); return 1; }
    cmvs_session_trace(s, o->trace);
    cmvs_session_budget(s, o->budget);
    /* The engine's own outer loop, at 0x0040CCCD: once round per frame. */
    rc = o->window ? run_window(s, o) : run_headless(s, o);
    cmvs_session_close(s);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
            "usage: %s <game folder> [--check [n] | --show <entry> | --bmp <entry> <out.bmp>]\n"
            "       %s <game folder> --run [script] [-v] [-f frames] [--shot out.png]\n"
            "                              [--window] [--point x y] [--click frame]...\n",
            argv[0], argv[0]);
        return 2;
    }
    if (argc >= 3 && !strcmp(argv[2], "--check")) {
        return cmd_check(argv[1], argc >= 4 ? atoi(argv[3]) : 40);
    }
    if (argc >= 3 && !strcmp(argv[2], "--scripts")) {
        return cmd_scripts(argv[1], argc >= 4 ? argv[3] : NULL);
    }
    if (argc >= 3 && !strcmp(argv[2], "--run")) {
        run_options o;
        int i;
        memset(&o, 0, sizeof o);
        o.script = NULL;
        o.budget = 2000000;
        o.frames = 60;
        o.hold = 1;
        for (i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "-v")) o.trace = 1;
            else if (!strcmp(argv[i], "-vv")) o.trace = 2;
            else if (!strcmp(argv[i], "-n") && i + 1 < argc) o.budget = atol(argv[++i]);
            else if (!strcmp(argv[i], "-f") && i + 1 < argc) o.frames = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--shot") && i + 1 < argc) o.shot = argv[++i];
            else if (!strcmp(argv[i], "--font") && i + 1 < argc) o.font = argv[++i];
            else if (!strcmp(argv[i], "--window")) { o.window = 1; o.frames = 0; }
            else if (!strcmp(argv[i], "--point") && i + 2 < argc) {
                o.point_x = atoi(argv[++i]);
                o.point_y = atoi(argv[++i]);
                o.has_point = 1;
            } else if (!strcmp(argv[i], "--click") && i + 1 < argc) {
                if (o.clicks < CLICKS) o.click_frame[o.clicks++] = atoi(argv[++i]);
                else i++;
            } else if (!strcmp(argv[i], "--hold") && i + 1 < argc) {
                o.hold = atoi(argv[++i]);
                if (o.hold < 1) o.hold = 1;
            }
            else o.script = argv[i];
        }
        return cmd_run(argv[1], &o);
    }
    if (argc >= 4 && !strcmp(argv[2], "--show")) return cmd_show(argv[1], argv[3]);
    if (argc >= 5 && !strcmp(argv[2], "--bmp")) return cmd_png(argv[1], argv[3], argv[4]);
    return cmd_list(argv[1]);
}
