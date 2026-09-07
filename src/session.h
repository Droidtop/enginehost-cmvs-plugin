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
                                const char *font, char *err, size_t errlen);
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

/*
 * Input. The pointer is in the GAME's coordinates, not the display's: a
 * frontend scales the picture onto its screen and so it, and only it, can undo
 * that. A session nobody points at simply never selects anything.
 * `button` is 0 for the left button (a tap, confirm) and 1 for the right
 * (cancel); `direction` is -1 up and +1 down.
 */
void cmvs_session_pointer(cmvs_session *s, int x, int y);
void cmvs_session_button(cmvs_session *s, int button, int down);
void cmvs_session_navigate(cmvs_session *s, int direction);

/*
 * Sound. The engine mixes and the frontend plays: open a device with two
 * channels of interleaved 16-bit samples at cmvs_session_audio_rate(), then
 * call cmvs_session_mix from that device's own thread for every block it
 * wants. Silence comes back when nothing is playing, so it is always safe to
 * call, and a frontend with no device simply never calls it.
 */
int cmvs_session_audio_rate(const cmvs_session *s);
void cmvs_session_mix(cmvs_session *s, int16_t *out, int frames);

/* What is running, for a status line or a log. */
const char *cmvs_session_script(const cmvs_session *s);
long cmvs_session_statements(const cmvs_session *s);
int cmvs_session_drawn(const cmvs_session *s);

/*
 * How many times a menu has answered a press with an item, and which item the
 * last one was. A frontend logs this: on a console it is the one thing that
 * separates "the tap never reached the engine" from "the menu saw it".
 */
int cmvs_session_menu_events(const cmvs_session *s, int *last_item);

/* Development handles: statement tracing and the command report. */
void cmvs_session_trace(cmvs_session *s, int on);
void cmvs_session_budget(cmvs_session *s, long statements_per_frame);
void cmvs_session_report(const cmvs_session *s, void *out);
int cmvs_session_unimplemented(const cmvs_session *s, int *distinct);

#endif
