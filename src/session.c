#include "session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "interp.h"
#include "font.h"
#include "scene.h"

struct cmvs_session {
    cmvs_game *game;
    cmvs_interp *interp;
    const uint8_t *pixels;
    cmvs_font *font;
    long budget;
    int alive;
};

/*
 * cmvs.cfg names its family in cp932; fontconfig is asked in UTF-8, and it is
 * asked through a shell, so a family with a quote in it never reaches it.
 */
static void family_utf8(const char *cp932, char *out, size_t outlen)
{
    size_t at = 0, put = 0;
    out[0] = 0;
    while (cp932 && cp932[at] && put + 4 < outlen) {
        unsigned code = cmvs_cp932_next(cp932, &at);
        if (!code || code == 0xFFFD) continue;
        if (code == 0x27 || code == 0x22 || code < 0x20) continue;
        if (code < 0x80) out[put++] = (char) code;
        else if (code < 0x800) {
            out[put++] = (char) (0xC0 | (code >> 6));
            out[put++] = (char) (0x80 | (code & 0x3F));
        } else {
            out[put++] = (char) (0xE0 | (code >> 12));
            out[put++] = (char) (0x80 | ((code >> 6) & 0x3F));
            out[put++] = (char) (0x80 | (code & 0x3F));
        }
    }
    out[put] = 0;
}

static void open_font(cmvs_session *s, const char *override)
{
    char wanted[256], path[1024];
    family_utf8(cmvs_game_font(s->game), wanted, sizeof wanted);
    if (!cmvs_font_find(override, wanted, path, sizeof path)) {
        /* Worth saying out loud: with no Japanese face on the machine the game
         * runs and every line of it is blank. */
        fprintf(stderr, "cmvs: no font for \"%s\": text will not draw\n", wanted);
        return;
    }
    s->font = cmvs_font_open(path);
    if (!s->font) fprintf(stderr, "cmvs: %s cannot be read as a font\n", path);
    else cmvs_scene_font(cmvs_interp_scene(s->interp), s->font);
}

cmvs_session *cmvs_session_open(const char *folder, const char *script,
                                const char *font, char *err, size_t errlen)
{
    cmvs_session *s = calloc(1, sizeof *s);
    if (!s) {
        if (err && errlen) snprintf(err, errlen, "out of memory");
        return NULL;
    }
    s->budget = 2000000;
    s->game = cmvs_game_open(folder, err, errlen);
    if (!s->game) { free(s); return NULL; }
    s->interp = cmvs_interp_new(s->game);
    if (!s->interp) {
        if (err && errlen) snprintf(err, errlen, "the interpreter could not be created");
        cmvs_game_close(s->game);
        free(s);
        return NULL;
    }
    if (!cmvs_interp_boot(s->interp,
                          script && *script ? script : cmvs_game_boot_script(s->game),
                          err, errlen)) {
        cmvs_session_close(s);
        return NULL;
    }
    open_font(s, font);
    s->alive = 1;
    return s;
}

void cmvs_session_close(cmvs_session *s)
{
    if (!s) return;
    cmvs_interp_free(s->interp);
    cmvs_font_free(s->font);
    cmvs_game_close(s->game);
    free(s);
}

int cmvs_session_width(const cmvs_session *s) { return s ? cmvs_game_width(s->game) : 0; }
int cmvs_session_height(const cmvs_session *s) { return s ? cmvs_game_height(s->game) : 0; }

int cmvs_session_frame(cmvs_session *s, char *err, size_t errlen)
{
    int rc;
    if (!s) return -1;
    rc = s->alive ? cmvs_interp_frame(s->interp, s->budget, err, errlen) : 0;
    if (rc <= 0) s->alive = 0;
    /* The scene is composed even on the frame the script ends on, so what the
     * last frame drew is still what is on screen. */
    s->pixels = cmvs_scene_compose(cmvs_interp_scene(s->interp), NULL, NULL);
    return rc;
}

const uint8_t *cmvs_session_pixels(const cmvs_session *s) { return s ? s->pixels : NULL; }

void cmvs_session_pointer(cmvs_session *s, int x, int y)
{
    if (s) cmvs_input_move(cmvs_interp_input(s->interp), x, y);
}

void cmvs_session_button(cmvs_session *s, int button, int down)
{
    if (s) cmvs_input_button(cmvs_interp_input(s->interp), button, down);
}

void cmvs_session_navigate(cmvs_session *s, int direction)
{
    if (s) cmvs_input_navigate(cmvs_interp_input(s->interp), direction);
}

const char *cmvs_session_script(const cmvs_session *s)
{
    return s ? cmvs_interp_script(s->interp) : "";
}

int cmvs_session_menu_events(const cmvs_session *s, int *last_item)
{
    if (!s) { if (last_item) *last_item = -1; return 0; }
    return cmvs_interp_menu_events(s->interp, last_item);
}

long cmvs_session_statements(const cmvs_session *s)
{
    return s ? cmvs_interp_statements(s->interp) : 0;
}

int cmvs_session_drawn(const cmvs_session *s)
{
    return s ? cmvs_scene_drawn(cmvs_interp_scene(s->interp)) : 0;
}

void cmvs_session_trace(cmvs_session *s, int on) { if (s) cmvs_interp_trace(s->interp, on); }

void cmvs_session_budget(cmvs_session *s, long statements_per_frame)
{
    if (s && statements_per_frame > 0) s->budget = statements_per_frame;
}

void cmvs_session_report(const cmvs_session *s, void *out)
{
    if (s) cmvs_interp_report(s->interp, out);
}

int cmvs_session_unimplemented(const cmvs_session *s, int *distinct)
{
    return s ? cmvs_interp_unimplemented(s->interp, distinct) : 0;
}
