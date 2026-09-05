/*
 * The running PS3 interpreter.
 *
 * vm.c decodes the bytecode; this executes it. The machine it models is the one
 * in cmvs32.exe: a byte-addressed data stack with a frame pointer per call
 * depth, one accumulator that every expression statement writes, a condition
 * flag the conditional jumps read, and a built-in command set the bytecode
 * calls two bytes at a time.
 *
 * Commands take their arguments from the top of the data stack WITHOUT popping
 * them - the bytecode pushes with 0x0430 and drops with 0x0412 itself. That is
 * why an unimplemented command is safe here exactly as it is in the original
 * engine, which leaves 264 of its 976 slots empty.
 */
#ifndef CMVS_INTERP_H
#define CMVS_INTERP_H

#include <stddef.h>
#include <stdint.h>

#include "game.h"
#include "script.h"

typedef struct cmvs_interp cmvs_interp;

/* Values are 32 bit. A string is the pool offset with this bit set, the same
 * tagging the engine uses (it masks handles with 0x3FFFFFFF at 0x4581FD). */
#define CMVS_STRING_TAG 0x80000000u

cmvs_interp *cmvs_interp_new(cmvs_game *game);
void cmvs_interp_free(cmvs_interp *in);

/* Loads a script into a slot and starts at its first index entry. */
int cmvs_interp_boot(cmvs_interp *in, const char *script, char *err, size_t errlen);

/*
 * Runs one frame: statements until a command yields (the engine reads bit
 * 0x8000 of a handler return as "stop", and command 0x000 returns 0xC000, which
 * is how every script ends a frame), or until `budget` statements have run.
 *
 * This is the shape of the original engine's outer loop: 0x0040CCCD calls the
 * interpreter once per frame, right after 0x00413610 has ticked the eight
 * layers. Returns 1 when the script is still alive and can be resumed next
 * frame, 0 when it ran off its end, -1 on an error (err says what).
 */
int cmvs_interp_frame(cmvs_interp *in, long budget, char *err, size_t errlen);

/* How much time one frame is worth, in milliseconds (default 16). The ten
 * engine timers advance by this at the head of every frame. */
void cmvs_interp_frame_ms(cmvs_interp *in, int ms);

/* The name of the script the interpreter is currently in. */
const char *cmvs_interp_script(const cmvs_interp *in);

void cmvs_interp_trace(cmvs_interp *in, int on);

/* What the run did, for the desktop runner to print. */
long cmvs_interp_statements(const cmvs_interp *in);
int cmvs_interp_unimplemented(const cmvs_interp *in, int *distinct);
void cmvs_interp_report(const cmvs_interp *in, void *out);

#endif
