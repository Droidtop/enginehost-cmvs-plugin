/*
 * The graphic objects the scripts draw with.
 *
 * The engine has two object families and only one of them is a sprite. The
 * table at +0xb90 is the eight display layers; the table at +0x77c is 256
 * GRAPHIC OBJECTS, and the title screen, the menus and the message window are
 * all built out of that second family.
 *
 * A graphic object is 0x2c94 bytes (constructed at 0x004328b0) and holds:
 *   - up to 0x300 child PARTS, themselves objects of the same class, created
 *     one at a time by command 0x022 (0x00433cb0);
 *   - one optional bitmap at +0xc10, decoded from a PB3 by command 0x030;
 *   - one optional DRAW ITEM at +0xc18, 0x88 bytes (0x0041b780), which is
 *     where all the geometry lives and which command 0x040 (for the object) or
 *     0x050 (for a part) creates.
 *
 * A part has no bitmap of its own: it draws a rectangle of its parent's. That
 * is how one sheet becomes a background and four buttons - ChronoClock's
 * title01_chip.pb3 is 1280x1024, the top 1280x720 is the title art and the
 * strip at y=722 holds the START / LOAD / SYSTEM / EXIT captions in their three
 * states, 300x60 apiece.
 */
#ifndef CMVS_SCENE_H
#define CMVS_SCENE_H

#include <stddef.h>
#include <stdint.h>

#include "game.h"

#define CMVS_OBJECTS 256      /* the table at +0x77c */
#define CMVS_PARTS   0x300    /* the bound every accessor checks */

typedef struct cmvs_scene cmvs_scene;

cmvs_scene *cmvs_scene_new(cmvs_game *game, int width, int height);
void cmvs_scene_free(cmvs_scene *s);

/* Commands 0x020 and 0x022: a fresh object, or a fresh part of one. Both
 * replace whatever was there, exactly as the engine's do. */
int cmvs_scene_object(cmvs_scene *s, int object);
int cmvs_scene_part(cmvs_scene *s, int object, int part);

/* Command 0x030: decode a PB3 and attach it. Returns 0 if the name is not in
 * any archive, which is what the command reports back to the script. */
int cmvs_scene_bitmap(cmvs_scene *s, int object, const char *name);

/* Commands 0x040 and 0x050: give the object, or one of its parts, a draw item.
 * `part` is -1 for the object itself, the convention the bytecode uses. */
int cmvs_scene_item(cmvs_scene *s, int object, int part);

/* The item setters. 0x044 is the source rectangle in the bitmap, 0x045 where it
 * lands on screen, 0x046 an offset applied to it, 0x047 a single size. */
void cmvs_scene_source(cmvs_scene *s, int object, int part, int x, int y, int w, int h);
void cmvs_scene_at(cmvs_scene *s, int object, int part, int x, int y);
void cmvs_scene_offset(cmvs_scene *s, int object, int part, int x, int y);
void cmvs_scene_size(cmvs_scene *s, int object, int part, int size);

/* Composes everything into one BGRA frame, top-down, stride 4 * width. The
 * buffer belongs to the scene. */
const uint8_t *cmvs_scene_compose(cmvs_scene *s, int *width, int *height);

/* What is on screen, for the runner to report without a window. */
int cmvs_scene_drawn(const cmvs_scene *s);

#endif
