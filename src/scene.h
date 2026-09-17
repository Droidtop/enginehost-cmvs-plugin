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
#include "input.h"
#include "input.h"
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

/*
 * The same, from a file already in memory rather than from an archive. The
 * original reaches it through 0x00420840 -> 0x0042fa50 -> 0x0042f2c0, which
 * sniffs four magics and nothing else (PNG, "BM", PB3B, MSK0); the LOAD
 * screen's slot thumbnails are the "BM" case, read by 0x0042ec80, and that
 * reader takes THREE fields - biWidth at 0x12, biHeight at 0x16, biBitCount at
 * 0x1c - refuses anything under 24 bpp and reads the pixels from a FIXED 0x36,
 * ignoring bfOffBits. A negative biHeight is a top-down bitmap.
 */
int cmvs_scene_bitmap_file(cmvs_scene *s, int object, int part,
                           const uint8_t *file, int size);

/*
 * Command 0x308 gives an object a MOVIE FRAME instead of a picture out of an
 * archive. The pixels are already BGRA top-down, so the object wears a copy of
 * them directly, and a frame whose size matches the one already there re-uses
 * that buffer rather than freeing and allocating 3.5 MB twenty-four times a
 * second.
 */
int cmvs_scene_bitmap_pixels(cmvs_scene *s, int object, int part,
                             int w, int h, const uint8_t *bgra, int stride);

/*
 * A blank plate of that size, which is what the LOAD screen draws for a slot
 * that has no thumbnail: 0x0046c2ea makes one at the size command 0x2b0 set
 * and hands it to 0x00434060 instead of the decoded picture.
 */
int cmvs_scene_bitmap_blank(cmvs_scene *s, int object, int part, int w, int h);

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

/* Commands 0x048 and 0x186 (0x0045fb80 and 0x00467c80, both -> 0x0041bdb0,
 * item +0x44): how opaque the item is, clamped ABOVE at 255 and not below, so
 * a script that runs its counter past zero simply leaves the item invisible.
 * This is what a CMVS transition is made of: the script itself walks the
 * value between 0 and 255 a frame at a time, there is no fade command. */
void cmvs_scene_alpha(cmvs_scene *s, int object, int part, int alpha);

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

/*
 * The other half of that table, and the one the choices are written in.
 * Command 0x100 (0x00463850) makes a text object of its own at +0xbb0[id] -
 * the same 0x164-byte class a layer keeps at its +0x9dc, with its own position
 * rather than a layer's - and the whole 0x100..0x11a family then addresses it
 * by that id, exactly as 0x141..0x15d addresses a layer's by layer. A choice
 * bar is one graphic object with a caption written into a text object made
 * this way, so without these the bar draws and the words do not.
 *
 * 0x100 replaces whatever the entry held, a layer's registration included,
 * because the original overwrites the pointer outright; 0x101 (0x00463B70)
 * drops it again.
 */
int cmvs_scene_text_create(cmvs_scene *s, int id);
void cmvs_scene_text_drop_id(cmvs_scene *s, int id);

/* One frame of time for every layer's reveal, so the typewriter runs off
 * the same clock the ten engine timers do. */
void cmvs_scene_text_tick(cmvs_scene *s, int ms);
cmvs_text *cmvs_scene_text_by_id(cmvs_scene *s, int id);

/* The face the glyphs are cut from; the session finds it, see font.h. */
void cmvs_scene_font(cmvs_scene *s, cmvs_font *font);

/*
 * A LOAD puts the picture back. The slot save carries every live graphic
 * object (record 0x700, one per object), every display layer (record 0x400)
 * and the drawing objects (0x380) as the engine's own serialisations, and
 * 0x0046FA10's reader re-creates each one from its record instead of replaying
 * the script that made it. These two are that reader, for the two record
 * shapes this engine models:
 *
 *   0x700 -> 0x0045DA5E: new object into +0x77c[i], then 0x00435710 on it
 *   0x400 -> 0x0045D46B: new layer into +0xb90[i], then 0x00451B80, which
 *                        skips the layer's own settings block and calls the
 *                        same 0x00435710 on the layer's object at +0x9d8
 *
 * `text_id` comes back with the word at the head of a layer record, which is
 * the entry of +0xbb0 that layer's text lives in (0x0045D565) - the same
 * registration command 0x15d makes while the script runs.
 *
 * Both answer 0 when the record is too short or does not have the shape the
 * original's reader expects. They never invent an object: a record that does
 * not parse leaves the slot empty rather than half-built.
 */
/*
 * A 0x380 record: one entry of the table at +0xbb0, which is a text object.
 * 0x0045D2D4 is the loader's handler for it - it makes the object, hands the
 * font name and the two reference numbers to 0x00420840, and then 0x00451950
 * reads the rest of the payload into it. This is that reader, for the fields
 * this engine's text object has:
 *
 *   u16 version; u32 (obj+0x138); char font[]; [u32 u32 when version >= 2]
 *   0x27 dwords  obj+0x04 .. obj+0xa0   - the position, box, size and colours
 *   0x25 dwords  obj+0xa4 .. obj+0x138  - the ruby and metrics half
 *   u16 count; count * 0x2c             - the glyphs already on the line
 *
 * The glyph entry is the ten-dword descriptor 0x004501B0 takes, prefixed by
 * the ordinal that routine writes back into it. Three of the ten are pinned
 * down - the character in the high half of the first, and the pen in the
 * fourth and fifth (0x00450EDE and 0x00450FA5 fill them from +0x98 and +0x9c
 * before the call) - and those are the three this engine keeps per glyph; the
 * other seven are *unproven* and are carried in the saved record rather than
 * invented. Both reference saves hold zero glyphs, so nothing here is checked
 * against a file that has any.
 */
int cmvs_scene_restore_text(cmvs_scene *s, int id, const uint8_t *data, int len);

int cmvs_scene_restore_object(cmvs_scene *s, int object, const uint8_t *data, int len);
int cmvs_scene_restore_layer(cmvs_scene *s, int layer, const uint8_t *data, int len,
                             int *text_id);

/*
 * THE LAYER'S OWN SPRITES, which is what the in-game toolbar is made of.
 *
 * A display layer is a 0x9e4-byte object of its own (allocated by command
 * 0x140 at 0x004665e0) and NOT a graphic object: the graphic object it draws
 * with hangs off it at +0x9d8. Inside it, at +0x28, is a table of 32 entries
 * of 0x4c bytes, and that table is a button bar:
 *
 *   entry + 0x00   on        1 while the sprite is registered (0x00452a00
 *                            sets it, 0x00452a40 clears it, and both show or
 *                            hide part (i + 0x10) of the layer's object with
 *                            it - so sprite i IS part i+16)
 *   entry + 0x04   five states of six words apiece, at +0x2c + 12*state:
 *                  sx, sy, sw, sh, ox, oy (0x00452960)
 *   entry + 0x40   x, y, w, h as u16 (0x004529c0), the HIT rectangle, taken
 *                  from the layer's own origin at +0xc/+0x10
 *   entry + 0x48   the state the sprite is wearing (0x004688d0 writes it and
 *                  0x004689a0 reads it)
 *
 * and the layer keeps the sprite a press began on at +0x9e0.
 *
 * 0x00452a80 is the poll over that table and it is the same contract
 * cmvs_menu_poll transcribes for the menu family: a press remembers where it
 * began, a release counts only where it began, and the pointer decides what
 * is under it. It answers three things - which sprite the pointer is over,
 * whether one was clicked, and whether one is held.
 */
#define CMVS_LAYER_SPRITES 0x20   /* the bound 0x00468870 and 0x004686a0 check */
#define CMVS_SPRITE_STATES 5      /* (0x68 - 0x2c) / 12, the room between the
                                   * states and the hit rectangle */
#define CMVS_LAYER_PART(sprite) ((sprite) + 0x10)  /* 0x00452a0b */

/* Commands 0x14e (0x00452550) and 0x14c (0x00452690): the script showing the
 * layer, and the player taking it away again. 0x14d reads the second flag. */
void cmvs_layer_present(cmvs_scene *s, int layer, int shown);
void cmvs_layer_hide(cmvs_scene *s, int layer, int hidden);
int cmvs_layer_hidden(const cmvs_scene *s, int layer);

/* Commands 0x190 and 0x191 (0x00452960, 0x004529c0). */
void cmvs_layer_sprite_state(cmvs_scene *s, int layer, int sprite, int state,
                             int sx, int sy, int sw, int sh, int ox, int oy);
void cmvs_layer_sprite_rect(cmvs_scene *s, int layer, int sprite,
                            int x, int y, int w, int h);

/* Commands 0x192 and 0x193 (0x00452a00, 0x00452a40): register or drop the
 * sprite, and show or hide the part that draws it with it. */
void cmvs_layer_sprite_show(cmvs_scene *s, int layer, int sprite, int on);

/* The sprite as it stands, for a test and for a log line: what the engine put
 * in the entry rather than what the script asked for. Answers 0 when the
 * sprite is not registered. */
int cmvs_layer_sprite_read(const cmvs_scene *s, int layer, int sprite,
                           int *x, int *y, int *w, int *h, int *worn);

/* Command 0x195 (0x00468870): is sprite i of this layer registered. */
int cmvs_layer_sprite_on(const cmvs_scene *s, int layer, int sprite);

/* Command 0x196 (0x004688d0): wear a state - which copies that state's source
 * rectangle onto part i+16, and does nothing at all when it is already worn. */
void cmvs_layer_sprite_wear(cmvs_scene *s, int layer, int sprite, int state);

/* Command 0x197 (0x004689a0): which state it is wearing. */
int cmvs_layer_sprite_worn(const cmvs_scene *s, int layer, int sprite);

/* Command 0x150 (0x00452750): where the layer's origin is. Mode 0 is absolute,
 * mode 1 adds to where it already is, and anything else takes the x for both
 * (which is the original's own fall-through, not a guess). */
void cmvs_layer_move(cmvs_scene *s, int layer, int mode, int x, int y);
void cmvs_layer_origin(const cmvs_scene *s, int layer, int *x, int *y);

/* Command 0x194 (0x00452a80). Returns whether the pointer is over any sprite;
 * *hover is the sprite it is over (-1 for none), *click whether one was
 * clicked, *held whether the button is down over one. It CONSUMES the confirm
 * edge exactly where the original does. */
int cmvs_layer_hit(cmvs_scene *s, int layer, cmvs_input *in,
                   int *hover, int *click, int *held);

/* Command 0x18d (0x00467be0): the draw item's position, order, alpha and the
 * two scales, for the layer's own object (part below zero) or one of its
 * parts. Every field is left as the caller set it when there is no item. */
void cmvs_scene_item_read(const cmvs_scene *s, int object, int part,
                          int *x, int *y, int *depth, int *alpha,
                          float *scale_x, float *scale_y);

/* Command 0x15f (0x004527c0): is the pointer inside this rectangle of the
 * layer, the layer's origin included. */
int cmvs_layer_hit_rect(const cmvs_scene *s, int layer, const cmvs_input *in,
                        int x, int y, int w, int h);

/*
 * THE LAYER'S OWN SPRITES, which is what the in-game toolbar is made of.
 *
 * A display layer is a 0x9e4-byte object of its own (allocated by command
 * 0x140 at 0x004665e0) and NOT a graphic object: the graphic object it draws
 * with hangs off it at +0x9d8. Inside it, at +0x28, is a table of 32 entries
 * of 0x4c bytes, and that table is a button bar:
 *
 *   entry + 0x00   on        1 while the sprite is registered (0x00452a00
 *                            sets it, 0x00452a40 clears it, and both show or
 *                            hide part (i + 0x10) of the layer's object with
 *                            it - so sprite i IS part i+16)
 *   entry + 0x04   five states of six words apiece, at +0x2c + 12*state:
 *                  sx, sy, sw, sh, ox, oy (0x00452960)
 *   entry + 0x40   x, y, w, h as u16 (0x004529c0), the HIT rectangle, taken
 *                  from the layer's own origin at +0xc/+0x10
 *   entry + 0x48   the state the sprite is wearing (0x004688d0 writes it and
 *                  0x004689a0 reads it)
 *
 * and the layer keeps the sprite a press began on at +0x9e0.
 *
 * 0x00452a80 is the poll over that table and it is the same contract
 * cmvs_menu_poll transcribes for the menu family: a press remembers where it
 * began, a release counts only where it began, and the pointer decides what
 * is under it. It answers three things - which sprite the pointer is over,
 * whether one was clicked, and whether one is held.
 */
#define CMVS_LAYER_SPRITES 0x20   /* the bound 0x00468870 and 0x004686a0 check */
#define CMVS_SPRITE_STATES 5      /* (0x68 - 0x2c) / 12, the room between the
                                   * states and the hit rectangle */
#define CMVS_LAYER_PART(sprite) ((sprite) + 0x10)  /* 0x00452a0b */

/* Commands 0x190 and 0x191 (0x00452960, 0x004529c0). */
void cmvs_layer_sprite_state(cmvs_scene *s, int layer, int sprite, int state,
                             int sx, int sy, int sw, int sh, int ox, int oy);
void cmvs_layer_sprite_rect(cmvs_scene *s, int layer, int sprite,
                            int x, int y, int w, int h);

/* Commands 0x192 and 0x193 (0x00452a00, 0x00452a40): register or drop the
 * sprite, and show or hide the part that draws it with it. */
void cmvs_layer_sprite_show(cmvs_scene *s, int layer, int sprite, int on);

/* Command 0x195 (0x00468870): is sprite i of this layer registered. */
int cmvs_layer_sprite_on(const cmvs_scene *s, int layer, int sprite);

/* Command 0x196 (0x004688d0): wear a state - which copies that state's source
 * rectangle onto part i+16, and does nothing at all when it is already worn. */
void cmvs_layer_sprite_wear(cmvs_scene *s, int layer, int sprite, int state);

/* Command 0x197 (0x004689a0): which state it is wearing. */
int cmvs_layer_sprite_worn(const cmvs_scene *s, int layer, int sprite);

/* Command 0x150 (0x00452750): where the layer's origin is. Mode 0 is absolute,
 * mode 1 adds to where it already is, and anything else takes the x for both
 * (which is the original's own fall-through, not a guess). */
void cmvs_layer_move(cmvs_scene *s, int layer, int mode, int x, int y);
void cmvs_layer_origin(const cmvs_scene *s, int layer, int *x, int *y);

/* Command 0x194 (0x00452a80). Returns whether the pointer is over any sprite;
 * *hover is the sprite it is over (-1 for none), *click whether one was
 * clicked, *held whether the button is down over one. It CONSUMES the confirm
 * edge exactly where the original does. */
int cmvs_layer_hit(cmvs_scene *s, int layer, cmvs_input *in,
                   int *hover, int *click, int *held);

/* Command 0x15f (0x004527c0): is the pointer inside this rectangle of the
 * layer, the layer's origin included. */
int cmvs_layer_hit_rect(const cmvs_scene *s, int layer, const cmvs_input *in,
                        int x, int y, int w, int h);

/* What is on screen, for the runner to report without a window. */
int cmvs_scene_drawn(const cmvs_scene *s);

#endif
