/*
 * One running game: the archives, the interpreter and the scene, opened and
 * stepped together.
 *
 * This is the whole engine as anything outside it needs to see it. The desktop
 * runner and the Android wrapper both drive a session and neither knows how the
 * three parts fit together, so there is one way to start a game rather than one
 * per frontend.
 */
#ifndef CMVS_SESSION_H
#define CMVS_SESSION_H

#include <stddef.h>
#include <stdint.h>

typedef struct cmvs_session cmvs_session;

/* Opens the game folder and boots `script`, or "start.ps3" when it is NULL -
 * the boot script every CMVS game ships loose beside its archives. */
cmvs_session *cmvs_session_open(const char *folder, const char *script,
                                char *err, size_t errlen);
void cmvs_session_close(cmvs_session *s);

int cmvs_session_width(const cmvs_session *s);
int cmvs_session_height(const cmvs_session *s);

/*
 * Runs one frame: statements until the script yields, then composes the scene.
 * Returns 1 while the script is alive, 0 once it has run off its end, -1 on an
 * error, with err saying what.
 */
int cmvs_session_frame(cmvs_session *s, char *err, size_t errlen);

/* The last composed frame: BGRA, top-down, stride 4 * width. */
const uint8_t *cmvs_session_pixels(const cmvs_session *s);

/* What is running, for a status line or a log. */
const char *cmvs_session_script(const cmvs_session *s);
long cmvs_session_statements(const cmvs_session *s);
int cmvs_session_drawn(const cmvs_session *s);

/* Development handles: statement tracing and the command report. */
void cmvs_session_trace(cmvs_session *s, int on);
void cmvs_session_budget(cmvs_session *s, long statements_per_frame);
void cmvs_session_report(const cmvs_session *s, void *out);
int cmvs_session_unimplemented(const cmvs_session *s, int *distinct);

#endif
