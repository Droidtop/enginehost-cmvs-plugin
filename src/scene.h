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

#include "camera.h"
#include "game.h"
#include "text.h"

#define CMVS_OBJECTS 256      /* the table at +0x77c */
#define CMVS_LAYERS  8        /* the table at +0xb90 */
#define CMVS_PARTS   0x300    /* the bound every accessor checks */
#define CMVS_TEXT_IDS 12      /* the table at +0xbb0, and 0x112's bound */

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

/* Command 0x033 (0x0045f1b0): the extent of the bitmap an object holds, which
 * is what a scene sizes its source rectangles from. */
int cmvs_scene_bitmap_size(cmvs_scene *s, int object, int part,
                           int *width, int *height, int *has_alpha);

/* Commands 0x040 and 0x050: give the object, or one of its parts, a draw item.
 * `part` is -1 for the object itself, the convention the bytecode uses. */
int cmvs_scene_item(cmvs_scene *s, int object, int part);

/* The item setters. 0x044 is the source rectangle in the bitmap, 0x045 where it
 * lands on screen, 0x046 an offset applied to it, 0x047 a single size. */
void cmvs_scene_source(cmvs_scene *s, int object, int part, int x, int y, int w, int h);
void cmvs_scene_at(cmvs_scene *s, int object, int part, int x, int y);
void cmvs_scene_offset(cmvs_scene *s, int object, int part, int x, int y);
/* Command 0x047 (0x0045fb10 -> 0x0041bda0, item +0x28): the DRAW ORDER, not a
 * size. snky01.ps3 gives its background 32 and the character sprite that
 * stands in front of it 99, and the scene is only right when the objects go
 * down in that order rather than by object number. */
void cmvs_scene_depth(cmvs_scene *s, int object, int part, int depth);

/* Command 0x215 shows a menu item's part (0x00432B70 sets +0xc38). A part
 * that has been hidden is not composed. */
void cmvs_scene_show(cmvs_scene *s, int object, int part, int visible);

/*
 * The staged half of the scene. Commands 0x042 (0x0041beb0) and 0x043
 * (0x0041bec0) turn an item from a flat one, placed by 0x045, into one the
 * camera places: kind 2 goes through camera 0 and kind 3 through camera 1,
 * and the compositor draws each kind in its own pass. The world position is
 * commands 0x072, 0x073 and 0x074; 0x070 is the depth at which the item is
 * drawn at its own size and 0x071 a lift added after the projection.
 */
void cmvs_scene_kind(cmvs_scene *s, int object, int part, int kind);
void cmvs_scene_world(cmvs_scene *s, int object, int part, int axis, float v);
void cmvs_scene_plane(cmvs_scene *s, int object, int part, float v);
void cmvs_scene_world_lift(cmvs_scene *s, int object, int part, float v);

/* The scene's cameras, for the 0x05x family and 0x062 to configure. */
cmvs_camera *cmvs_scene_camera(cmvs_scene *s, int index);

/* Composes everything into one BGRA frame, top-down, stride 4 * width. The
 * buffer belongs to the scene. */
const uint8_t *cmvs_scene_compose(cmvs_scene *s, int *width, int *height);

/*
 * The text a layer draws (the layer object's own +0x9dc). The scene owns it
 * because the scene is what composes it: a layer's line sits inside the window
 * that layer draws, so it lands at the layer object's own position.
 */
cmvs_text *cmvs_scene_text(cmvs_scene *s, int layer);

/*
 * The same text object under the id a script measures it by. Command 0x15d
 * registers a layer's text object in the table at +0xbb0 (ChronoClock's message
 * window is layer 0 under id 7) and command 0x112 then measures by that id, so
 * the two commands must reach one object or the script lays its window out from
 * numbers that belong to nothing.
 */
void cmvs_scene_text_register(cmvs_scene *s, int id, int layer);

/* One frame of time for every layer's reveal, so the typewriter runs off
 * the same clock the ten engine timers do. */
void cmvs_scene_text_tick(cmvs_scene *s, int ms);
cmvs_text *cmvs_scene_text_by_id(cmvs_scene *s, int id);

/* The face the glyphs are cut from; the session finds it, see font.h. */
void cmvs_scene_font(cmvs_scene *s, cmvs_font *font);

/* What is on screen, for the runner to report without a window. */
int cmvs_scene_drawn(const cmvs_scene *s);

#endif
