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
 *
 * The EIGHT DISPLAY LAYERS are the same family, not a second one. 0x00451d30
 * is how every layer command reaches what it acts on, and all it does is take
 * the layer's own graphic object at +0x9d8 and, for a sprite id of 0 or more,
 * ask it for that child part through 0x00432bd0 - the very call the object
 * commands use. So "sprite N on layer L" is "part N of the object belonging to
 * layer L", and a sprite id of -1 is the layer object itself, exactly as -1
 * means the object itself in the 0x04x commands. The layers live at the end of
 * the same table here so that one set of accessors serves both.
 */
#ifndef CMVS_SCENE_H
#define CMVS_SCENE_H

#include <stddef.h>
#include <stdint.h>

#include "game.h"

#define CMVS_OBJECTS 256      /* the table at +0x77c */
#define CMVS_LAYERS  8        /* the table at +0xb90 */
#define CMVS_PARTS   0x300    /* the bound every accessor checks */

/* Where layer L's graphic object lives in the same table. Every layer command
 * turns its layer argument into this and then speaks the object vocabulary. */
#define CMVS_LAYER_OBJECT(layer) (CMVS_OBJECTS + (layer))

typedef struct cmvs_scene cmvs_scene;

cmvs_scene *cmvs_scene_new(cmvs_game *game, int width, int height);
void cmvs_scene_free(cmvs_scene *s);

/* Commands 0x020 and 0x022: a fresh object, or a fresh part of one. Both
 * replace whatever was there, exactly as the engine's do. */
int cmvs_scene_object(cmvs_scene *s, int object);
int cmvs_scene_part(cmvs_scene *s, int object, int part);

/* Commands 0x021 (0x0045e9b0), 0x02f (0x0045ea10) and 0x179 (0x00451e30): the
 * other half of creating one. A part index of -1 drops the object itself, and
 * with it every part it holds. */
void cmvs_scene_drop(cmvs_scene *s, int object, int part);
void cmvs_scene_drop_all(cmvs_scene *s);

/* Command 0x027 (0x0045eb50): whether that object, or that part of it, is
 * there. The script asks before it draws. */
int cmvs_scene_exists(const cmvs_scene *s, int object, int part);

/* Commands 0x17c (0x004337a0): the object's own extent, +0xc44 and +0xc48. It
 * is what a layer object is drawn at when it has no source rectangle of its
 * own, which is how the message window covers the screen. */
void cmvs_scene_extent(cmvs_scene *s, int object, int part, int w, int h);

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

/* Command 0x215 shows a menu item's part (0x00432B70 sets +0xc38). A part
 * that has been hidden is not composed. */
void cmvs_scene_show(cmvs_scene *s, int object, int part, int visible);

/* Composes everything into one BGRA frame, top-down, stride 4 * width. The
 * buffer belongs to the scene. */
const uint8_t *cmvs_scene_compose(cmvs_scene *s, int *width, int *height);

/* What is on screen, for the runner to report without a window. */
int cmvs_scene_drawn(const cmvs_scene *s);

#endif
