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

/*
 * Opens the game folder and boots `script`, or "start.ps3" when it is NULL -
 * the boot script every CMVS game ships loose beside its archives.
 *
 * `saves` is the folder the HOST keeps this game's saves in. The boot script
 * names a subfolder inside it (command 0x016) and the files written there are
 * the game's own: byte-compatible CSV2 slots and a CSS1 system file, under the
 * names the Windows game uses, so a slot copies either way. Pass NULL and the
 * engine has no save folder at all - it says so and saves nothing. It never
 * writes into the game folder.
 */
cmvs_session *cmvs_session_open(const char *folder, const char *script,
                                const char *font, const char *saves,
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

/* Says what rate the frontend's device opened at, which is the device's answer
 * and not the frontend's request. Everything already playing is resampled to
 * it from the next block on. */
void cmvs_session_audio_open(cmvs_session *s, int rate);

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

/*
 * KEY_FUNCTION 13 and 14, quick save and quick load. They are the engine's own
 * named actions (see coordination agents/cmvs/KEY-FUNCTIONS.md) and they act on
 * slot 999, which is what the original calls the quick slot - the same file the
 * Windows game's QUICK SAVE writes. Both answer 0 with a reason when there is
 * no save folder or no such slot.
 *
 * The other three of that group - 15 popup menu, 19 save screen, 20 load screen
 * - are not here: in the original they are polled by the SCRIPT, through the
 * accessors at 0x00448B20, and the command that does that polling is not
 * implemented yet. There is nothing for the engine itself to do with them.
 */
int cmvs_session_quick_save(cmvs_session *s, char *err, size_t errlen);
int cmvs_session_quick_load(cmvs_session *s, char *err, size_t errlen);

/* Any slot; the two above are this on slot 999. The Data Save and Data Load
 * screens reach these through the script, by way of commands 0x2b6 and
 * 0x2b5. */
int cmvs_session_save_slot(cmvs_session *s, int slot, char *err, size_t errlen);
int cmvs_session_load_slot(cmvs_session *s, int slot, char *err, size_t errlen);

/* The folder the saves are actually being kept in, once the boot script has
 * named it, or NULL. A console run's log needs this to be readable. */
const char *cmvs_session_save_folder(const cmvs_session *s);

/* Development handles: statement tracing and the command report. */
void cmvs_session_trace(cmvs_session *s, int on);
void cmvs_session_budget(cmvs_session *s, long statements_per_frame);
void cmvs_session_report(const cmvs_session *s, void *out);
int cmvs_session_unimplemented(const cmvs_session *s, int *distinct);

#endif
