#include "session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "interp.h"
#include "scene.h"

struct cmvs_session {
    cmvs_game *game;
    cmvs_interp *interp;
    const uint8_t *pixels;
    long budget;
    int alive;
};

cmvs_session *cmvs_session_open(const char *folder, const char *script,
                                char *err, size_t errlen)
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
    if (!cmvs_interp_boot(s->interp, script && *script ? script : "start.ps3", err, errlen)) {
        cmvs_session_close(s);
        return NULL;
    }
    s->alive = 1;
    return s;
}

void cmvs_session_close(cmvs_session *s)
{
    if (!s) return;
    cmvs_interp_free(s->interp);
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
