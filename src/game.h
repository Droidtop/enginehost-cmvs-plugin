/*
 * One opened CMVS game: its configuration, its archives, and the lookups the
 * interpreter needs.
 *
 * The engine addresses its data by the names the bytecode carries, not by a
 * path: a script is "intproc.ps3", an image "title.pb3". Which archive holds a
 * name is not written down anywhere, so the lookup searches them.
 */
#ifndef CMVS_GAME_H
#define CMVS_GAME_H

#include <stddef.h>
#include <stdint.h>

#include "cpz.h"
#include "pb3.h"
#include "script.h"

typedef struct cmvs_game cmvs_game;

/* Reads cmvs.cfg for the pack folder and the window size, then opens every
 * archive it finds there. */
cmvs_game *cmvs_game_open(const char *folder, char *err, size_t errlen);
void cmvs_game_close(cmvs_game *g);

int cmvs_game_width(const cmvs_game *g);
int cmvs_game_height(const cmvs_game *g);

/*
 * Loads a script by the name the bytecode uses. A loose file in the pack folder
 * wins, because that is where start.ps3 lives; otherwise it is "code/<name>"
 * inside script.cpz. The caller closes the script.
 */
int cmvs_game_script(cmvs_game *g, const char *name, cmvs_script *out,
                     char *err, size_t errlen);

/* Decodes an image by entry name, searching every archive that holds images. */
int cmvs_game_image(cmvs_game *g, const char *name, pb3_image *out,
                    char *err, size_t errlen);

#endif
