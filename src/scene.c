#include "scene.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The draw item at +0xc18. Every field here was read off one of its setters, so
 * the ones with no setter yet simply are not here.
 *
 *   +0x04  a mode, 0..3 (0x0041bdd0), 1 out of the constructor
 *   +0x08  +0x0c  +0x10  +0x14   x, y, w, h    (0x0041bd00, command 0x044)
 *   +0x18  +0x1c                 an offset     (0x0041bd40, command 0x046)
 *   +0x20  +0x24                 the position  (0x0041bd60, command 0x045)
 *   +0x28                        a size        (0x0041bda0, command 0x047)
 *   +0x44                        alpha, clamped to 255 (0x0041bdb0)
 *   +0x54                        an opacity out of 0x100
 */
typedef struct {
    int used;
    int visible;
    int mode;
    int sx, sy, sw, sh;
    int ox, oy;
    int x, y;
    int depth;                   /* +0x28: the draw order, command 0x047 */
    int alpha;
    int opacity;
    /*
     * +0x00, and with it the other half of the item. Zero is the flat item
     * the constructor leaves behind, placed by its own 0x045; 2 and 3 are what
     * commands 0x042 and 0x043 write, and they mean the camera places it.
     */
    int kind;
    cmvs_placement world;        /* +0x2c..+0x40, +0x48, +0x4c */
} cmvs_item;

typedef struct cmvs_object {
    cmvs_item item;
    pb3_image bitmap;
    int has_bitmap;
    int ew, eh;                  /* +0xc44, +0xc48: the object's own extent */
    struct cmvs_object **part;   /* CMVS_PARTS pointers, allocated with the object */
} cmvs_object;

/*
 * A DISPLAY LAYER, the 0x9e4-byte object command 0x140 allocates at
 * 0x004665e0. Only the half this engine has anything to do with is here: the
 * origin at +0xc/+0x10, the 32-entry sprite table at +0x28 and the sprite a
 * press began on at +0x9e0. The graphic object the layer draws with is the
 * one at +0x9d8, which lives in the same table as the 256 script objects.
 */
typedef struct {
    int on;                                  /* entry +0x00 */
    int sx[CMVS_SPRITE_STATES], sy[CMVS_SPRITE_STATES];
    int sw[CMVS_SPRITE_STATES], sh[CMVS_SPRITE_STATES];
    int ox[CMVS_SPRITE_STATES], oy[CMVS_SPRITE_STATES];   /* entry +0x2c */
    int x, y, w, h;                          /* entry +0x68, the hit rectangle */
    int worn;                                /* entry +0x70 */
} cmvs_sprite;

typedef struct {
    int shown;                               /* +0x04, command 0x14e */
    int hidden;                              /* +0x08, command 0x14c */
    int x, y;                                /* +0x0c, +0x10 */
    cmvs_sprite sprite[CMVS_LAYER_SPRITES];  /* +0x28 */
    int press_from;                          /* +0x9e0 */
} cmvs_layer;

struct cmvs_scene {
    cmvs_game *game;
    int width, height;
    uint8_t *frame;              /* BGRA, 4 * width * height */
    /* The 256 graphic objects, then the 8 layer objects: one table, because
     * 0x00451d30 reaches a layer's sprites with the object family's own
     * accessor and there is nothing to tell apart below that call. */
    cmvs_object *object[CMVS_OBJECTS + CMVS_LAYERS];
    cmvs_camera camera[CMVS_CAMERAS];
    cmvs_layer layer[CMVS_LAYERS];
    cmvs_text text[CMVS_LAYERS];
    /*
     * The table at +0xbb0 holds twelve text objects. An entry is either a
     * layer's own (registered by command 0x15d, `of_id` says which layer) or
     * one command 0x100 made for the script alone, which lives here and is
     * placed by its own position rather than by a layer's.
     */
    int of_id[CMVS_TEXT_IDS];
    cmvs_text id_text[CMVS_TEXT_IDS];
    int id_used[CMVS_TEXT_IDS];
    cmvs_font *font;
    int drawn;                   /* how many items the last compose blitted */
};

static cmvs_object *object_new(void)
{
    cmvs_object *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->part = calloc(CMVS_PARTS, sizeof *o->part);
    if (!o->part) { free(o); return NULL; }
    o->item.alpha = 0xFF;
    o->item.opacity = 0x100;
    o->item.mode = 1;
    o->item.visible = 1;
    o->item.world.scale_x = 1.0f;
    o->item.world.scale_y = 1.0f;
    return o;
}

static void object_free(cmvs_object *o)
{
    int i;
    if (!o) return;
    for (i = 0; i < CMVS_PARTS; i++) object_free(o->part[i]);
    if (o->has_bitmap) pb3_free(&o->bitmap);
    free(o->part);
    free(o);
}

cmvs_scene *cmvs_scene_new(cmvs_game *game, int width, int height)
{
    cmvs_scene *s;
    int i;
    if (width <= 0 || height <= 0) return NULL;
    s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->game = game;
    s->width = width;
    s->height = height;
    s->frame = calloc((size_t) width * height, 4);
    if (!s->frame) { free(s); return NULL; }
    for (i = 0; i < CMVS_LAYERS; i++) cmvs_text_init(&s->text[i]);
    for (i = 0; i < CMVS_TEXT_IDS; i++) s->of_id[i] = -1;
    for (i = 0; i < CMVS_CAMERAS; i++) cmvs_camera_init(&s->camera[i]);
    /* +0x9e0 starts at -1: 0x00452a80 reads it as "no press is in flight". */
    for (i = 0; i < CMVS_LAYERS; i++) s->layer[i].press_from = -1;
    return s;
}

void cmvs_scene_free(cmvs_scene *s)
{
    int i;
    if (!s) return;
    for (i = 0; i < CMVS_OBJECTS + CMVS_LAYERS; i++) object_free(s->object[i]);
    for (i = 0; i < CMVS_LAYERS; i++) cmvs_text_free(&s->text[i]);
    for (i = 0; i < CMVS_TEXT_IDS; i++) cmvs_text_free(&s->id_text[i]);
    free(s->frame);
    free(s);
}

/* The object a command means, with -1 for "the object itself" - the same test
 * every one of the setters makes before it calls 0x00432bd0. */
static cmvs_object *reach(cmvs_scene *s, int object, int part)
{
    cmvs_object *o;
    if (object < 0 || object >= CMVS_OBJECTS + CMVS_LAYERS) return NULL;
    o = s->object[object];
    if (!o || part < 0) return o;
    if (part >= CMVS_PARTS) return NULL;
    return o->part[part];
}

int cmvs_scene_object(cmvs_scene *s, int object)
{
    if (object < 0 || object >= CMVS_OBJECTS + CMVS_LAYERS) return 0;
    object_free(s->object[object]);
    s->object[object] = object_new();
    return s->object[object] != NULL;
}

int cmvs_scene_part(cmvs_scene *s, int object, int part)
{
    cmvs_object *o;
    if (object < 0 || object >= CMVS_OBJECTS + CMVS_LAYERS
        || part < 0 || part >= CMVS_PARTS) return 0;
    o = s->object[object];
    if (!o) return 0;
    object_free(o->part[part]);
    o->part[part] = object_new();
    return o->part[part] != NULL;
}

void cmvs_scene_drop(cmvs_scene *s, int object, int part)
{
    if (object < 0 || object >= CMVS_OBJECTS + CMVS_LAYERS) return;
    if (part < 0) {
        object_free(s->object[object]);
        s->object[object] = NULL;
        return;
    }
    if (!s->object[object] || part >= CMVS_PARTS) return;
    object_free(s->object[object]->part[part]);
    s->object[object]->part[part] = NULL;
}

void cmvs_scene_drop_all(cmvs_scene *s)
{
    int i;
    /* 0x0045ea10 walks the 256 graphic objects and nothing else: the layers
     * keep what they hold, which is why clearing the interface does not clear
     * the scene under it. */
    for (i = 0; i < CMVS_OBJECTS; i++) {
        object_free(s->object[i]);
        s->object[i] = NULL;
    }
}

int cmvs_scene_exists(const cmvs_scene *s, int object, int part)
{
    if (object < 0 || object >= CMVS_OBJECTS + CMVS_LAYERS) return 0;
    if (!s->object[object]) return 0;
    if (part < 0) return 1;
    if (part >= CMVS_PARTS) return 0;
    return s->object[object]->part[part] != NULL;
}

int cmvs_scene_bitmap(cmvs_scene *s, int object, const char *name)
{
    cmvs_object *o = reach(s, object, -1);
    pb3_image img;
    char err[256];
    if (!o || !name || !*name) return 0;
    if (!cmvs_game_image(s->game, name, &img, err, sizeof err)) return 0;
    if (o->has_bitmap) pb3_free(&o->bitmap);
    o->bitmap = img;
    o->has_bitmap = 1;
    return 1;
}

/* Puts a decoded picture on the object or part, freeing whatever was there. */
static int wear_bitmap(cmvs_scene *s, int object, int part, pb3_image *img)
{
    cmvs_object *o = reach(s, object, part);
    if (!o) { pb3_free(img); return 0; }
    if (o->has_bitmap) pb3_free(&o->bitmap);
    o->bitmap = *img;
    o->has_bitmap = 1;
    return 1;
}

int cmvs_scene_bitmap_file(cmvs_scene *s, int object, int part,
                           const uint8_t *file, int size)
{
    pb3_image img;
    int w, h, bpp, top_down, row, col, stride;
    if (!file || size < 0x36) return 0;
    if (file[0] != 'B' || file[1] != 'M') return 0;
    w = (int) ((uint32_t) file[0x12] | ((uint32_t) file[0x13] << 8)
               | ((uint32_t) file[0x14] << 16) | ((uint32_t) file[0x15] << 24));
    h = (int) ((uint32_t) file[0x16] | ((uint32_t) file[0x17] << 8)
               | ((uint32_t) file[0x18] << 16) | ((uint32_t) file[0x19] << 24));
    bpp = file[0x1c] | (file[0x1d] << 8);
    if (bpp != 24 && bpp != 32) return 0;
    top_down = h < 0;
    if (top_down) h = -h;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return 0;
    /* Rows are padded to a four-byte boundary, which is the only thing about
     * the layout the reader does not take from a header field. */
    stride = (w * (bpp / 8) + 3) & ~3;
    if ((long) size - 0x36 < (long) stride * h) return 0;
    img.width = w;
    img.height = h;
    img.has_alpha = 0;
    img.pixels = malloc((size_t) w * h * 4);
    if (!img.pixels) return 0;
    for (row = 0; row < h; row++) {
        const uint8_t *src = file + 0x36 + (size_t) stride * (top_down ? row : h - 1 - row);
        uint8_t *dst = img.pixels + (size_t) row * w * 4;
        for (col = 0; col < w; col++) {
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = 0xFF;
            src += bpp / 8;
            dst += 4;
        }
    }
    return wear_bitmap(s, object, part, &img);
}

int cmvs_scene_bitmap_pixels(cmvs_scene *s, int object, int part,
                             int w, int h, const uint8_t *bgra, int stride)
{
    cmvs_object *o = reach(s, object, part);
    pb3_image img;
    int row;
    if (!o || !bgra || w <= 0 || h <= 0 || stride < 4 * w) return 0;
    if (o->has_bitmap && o->bitmap.width == w && o->bitmap.height == h
        && o->bitmap.pixels) {
        for (row = 0; row < h; row++)
            memcpy(o->bitmap.pixels + (size_t) row * w * 4,
                   bgra + (size_t) row * stride, (size_t) w * 4);
        o->bitmap.has_alpha = 0;
        return 1;
    }
    img.width = w;
    img.height = h;
    img.has_alpha = 0;
    img.pixels = malloc((size_t) w * h * 4);
    if (!img.pixels) return 0;
    for (row = 0; row < h; row++)
        memcpy(img.pixels + (size_t) row * w * 4, bgra + (size_t) row * stride,
               (size_t) w * 4);
    return wear_bitmap(s, object, part, &img);
}

int cmvs_scene_bitmap_blank(cmvs_scene *s, int object, int part, int w, int h)
{
    pb3_image img;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return 0;
    img.width = w;
    img.height = h;
    img.has_alpha = 0;
    img.pixels = calloc((size_t) w * h, 4);
    if (!img.pixels) return 0;
    return wear_bitmap(s, object, part, &img);
}

/*
 * Command 0x033 (0x0045f1b0) asks an object how big its bitmap is: the object
 * at +0x77c is reached, its image queried, and the width at +0x68, the height
 * at +0x6c and "has an alpha channel" at +0x70 are written into the system
 * values. The scene's scripts SIZE THEMSELVES from the answer - snky01.ps3
 * asks before it fills in the source rectangle of a background or a character
 * sprite - so without it the rectangle came from whatever the previous command
 * had left behind, and the summer-sky background drew six pixels wide.
 */
int cmvs_scene_bitmap_size(cmvs_scene *s, int object, int part,
                           int *width, int *height, int *has_alpha)
{
    cmvs_object *o = reach(s, object, part);
    if (!o || !o->has_bitmap) return 0;
    if (width) *width = o->bitmap.width;
    if (height) *height = o->bitmap.height;
    if (has_alpha) *has_alpha = o->bitmap.has_alpha ? 1 : 0;
    return 1;
}

int cmvs_scene_item(cmvs_scene *s, int object, int part)
{
    cmvs_object *o = reach(s, object, part);
    if (!o) return 0;
    memset(&o->item, 0, sizeof o->item);
    o->item.used = 1;
    o->item.mode = 1;
    o->item.visible = 1;
    o->item.alpha = 0xFF;
    o->item.opacity = 0x100;
    o->item.world.scale_x = 1.0f;
    o->item.world.scale_y = 1.0f;
    return 1;
}

void cmvs_scene_show(cmvs_scene *s, int object, int part, int visible)
{
    cmvs_object *o = reach(s, object, part);
    if (!o) return;
    o->item.visible = visible ? 1 : 0;
}

void cmvs_scene_source(cmvs_scene *s, int object, int part, int x, int y, int w, int h)
{
    cmvs_object *o = reach(s, object, part);
    if (!o) return;
    o->item.sx = x;
    o->item.sy = y;
    o->item.sw = w;
    o->item.sh = h;
}

void cmvs_scene_at(cmvs_scene *s, int object, int part, int x, int y)
{
    cmvs_object *o = reach(s, object, part);
    if (!o) return;
    o->item.x = x;
    o->item.y = y;
}

void cmvs_scene_offset(cmvs_scene *s, int object, int part, int x, int y)
{
    cmvs_object *o = reach(s, object, part);
    if (!o) return;
    o->item.ox = x;
    o->item.oy = y;
}

void cmvs_scene_extent(cmvs_scene *s, int object, int part, int w, int h)
{
    cmvs_object *o = reach(s, object, part);
    if (!o) return;
    o->ew = w;
    o->eh = h;
}

void cmvs_scene_depth(cmvs_scene *s, int object, int part, int depth)
{
    cmvs_object *o = reach(s, object, part);
    if (!o) return;
    o->item.depth = depth;
}

void cmvs_scene_alpha(cmvs_scene *s, int object, int part, int alpha)
{
    cmvs_object *o = reach(s, object, part);
    if (!o) return;
    o->item.alpha = alpha > 255 ? 255 : alpha;
}

void cmvs_scene_kind(cmvs_scene *s, int object, int part, int kind)
{
    cmvs_object *o = reach(s, object, part);
    if (!o) return;
    o->item.kind = kind;
}

void cmvs_scene_world(cmvs_scene *s, int object, int part, int axis, float v)
{
    cmvs_object *o = reach(s, object, part);
    if (!o) return;
    if (axis == 0) o->item.world.x = v;
    else if (axis == 1) o->item.world.y = v;
    else o->item.world.z = v;
}

void cmvs_scene_plane(cmvs_scene *s, int object, int part, float v)
{
    cmvs_object *o = reach(s, object, part);
    if (!o) return;
    o->item.world.plane = v;
}

void cmvs_scene_world_lift(cmvs_scene *s, int object, int part, float v)
{
    cmvs_object *o = reach(s, object, part);
    if (!o) return;
    o->item.world.lift = v;
}

cmvs_camera *cmvs_scene_camera(cmvs_scene *s, int index)
{
    if (!s || index < 0 || index >= CMVS_CAMERAS) return NULL;
    return &s->camera[index];
}

cmvs_text *cmvs_scene_text(cmvs_scene *s, int layer)
{
    if (!s || layer < 0 || layer >= CMVS_LAYERS) return NULL;
    return &s->text[layer];
}

void cmvs_scene_text_register(cmvs_scene *s, int id, int layer)
{
    if (!s || id < 0 || id >= CMVS_TEXT_IDS) return;
    s->of_id[id] = (layer >= 0 && layer < CMVS_LAYERS) ? layer : -1;
    if (s->of_id[id] >= 0) cmvs_scene_text_drop_id(s, id);
}

int cmvs_scene_text_create(cmvs_scene *s, int id)
{
    if (!s || id < 0 || id >= CMVS_TEXT_IDS) return 0;
    cmvs_text_free(&s->id_text[id]);
    cmvs_text_init(&s->id_text[id]);
    s->id_used[id] = 1;
    s->of_id[id] = -1;
    return 1;
}

void cmvs_scene_text_drop_id(cmvs_scene *s, int id)
{
    if (!s || id < 0 || id >= CMVS_TEXT_IDS) return;
    cmvs_text_free(&s->id_text[id]);
    s->id_used[id] = 0;
}

void cmvs_scene_text_tick(cmvs_scene *s, int ms)
{
    int i;
    if (!s) return;
    for (i = 0; i < CMVS_LAYERS; i++) cmvs_text_tick(&s->text[i], ms);
    for (i = 0; i < CMVS_TEXT_IDS; i++)
        if (s->id_used[i]) cmvs_text_tick(&s->id_text[i], ms);
}

cmvs_text *cmvs_scene_text_by_id(cmvs_scene *s, int id)
{
    if (!s || id < 0 || id >= CMVS_TEXT_IDS) return NULL;
    if (s->of_id[id] >= 0) return &s->text[s->of_id[id]];
    return s->id_used[id] ? &s->id_text[id] : NULL;
}

void cmvs_scene_font(cmvs_scene *s, cmvs_font *font) { if (s) s->font = font; }

/* ------------------------------------------------------- a load's picture
 *
 * 0x00435710 is the engine's own reader for a graphic object, and everything
 * below is that routine and nothing else. A record is a small fixed head, then
 * optional TAGGED BLOCKS in a fixed order, then the object's child parts, each
 * one a record of the same shape. Every block header the routine reads is a
 * u16 tag and a u32 length, and it only ever tests the tag it is expecting
 * next - so a block that is not there costs nothing and a block this engine
 * does not model is stepped over by its own length.
 *
 * The head, at 0x0043574D..0x004357C4:
 *   u16 0xFFFF, u16 version   - present only when the first word is 0xFFFF,
 *                               which is how an unversioned record (version 0)
 *                               is told from a versioned one
 *   i32 +0xc34, i32 +0xc38, i32 +0xc3c
 *   version >= 1: i32 +0xc1c, i32 +0xc28, i32 +0xc2c
 *   version >= 3: i32 +0xc44, i32 +0xc48   (the object's extent)
 * then the blocks 0x740 (+0xc10, the bitmap), 0x741 (+0xc14), 0x745 (+0xc30),
 * 0x749 (+0xc20) and 0x780 (+0xc18, the draw item), then for version >= 2 an
 * eleven-dword tail, then the parts.
 *
 * +0xc38 is what command 0x215 writes through 0x00432B70, so it is the
 * object SHOWN flag and it is applied as one.
 */

static int rd_u16(const uint8_t *d, int len, int at, unsigned *out)
{
    if (at < 0 || at + 2 > len) return 0;
    *out = (unsigned) d[at] | ((unsigned) d[at + 1] << 8);
    return 1;
}

static int rd_i32(const uint8_t *d, int len, int at, int32_t *out)
{
    if (at < 0 || at + 4 > len) return 0;
    *out = (int32_t) ((uint32_t) d[at] | ((uint32_t) d[at + 1] << 8)
                    | ((uint32_t) d[at + 2] << 16) | ((uint32_t) d[at + 3] << 24));
    return 1;
}

static float rd_f32(const uint8_t *d, int len, int at)
{
    int32_t bits = 0;
    float v;
    if (!rd_i32(d, len, at, &bits)) return 0.0f;
    memcpy(&v, &bits, 4);
    return v;
}

/*
 * The draw item, 0x0041B980. Its head is a u32 version and then TWENTY-TWO
 * DWORDS copied straight over the item (0x0041B9BD is a rep movsd of 0x16),
 * which is why every field below is read at the item offset itself rather than
 * at one of its own. Two of them are not taken from the record: 0x0041B9C4
 * writes 0x100 into +0x54 unconditionally, and the alpha at +0x44 keeps the
 * clamp its setter has.
 *
 * What follows the dwords - +0x58..+0x68 for version 2 and up, a float at
 * +0x6c for version 4, and the 0x790/0x791/0x792 sub-objects - is not modelled
 * by this engine, so it is read past rather than applied. The block own
 * length is what steps over it.
 */
static void restore_item(cmvs_item *it, const uint8_t *d, int len)
{
    int32_t v = 0;
    if (len < 4 + 0x58) return;
    memset(it, 0, sizeof *it);
    it->used = 1;
    it->visible = 1;
    d += 4;
    len -= 4;
    rd_i32(d, len, 0x00, &v); it->kind = v;
    rd_i32(d, len, 0x04, &v); it->mode = v;
    rd_i32(d, len, 0x08, &v); it->sx = v;
    rd_i32(d, len, 0x0C, &v); it->sy = v;
    rd_i32(d, len, 0x10, &v); it->sw = v;
    rd_i32(d, len, 0x14, &v); it->sh = v;
    rd_i32(d, len, 0x18, &v); it->ox = v;
    rd_i32(d, len, 0x1C, &v); it->oy = v;
    rd_i32(d, len, 0x20, &v); it->x = v;
    rd_i32(d, len, 0x24, &v); it->y = v;
    rd_i32(d, len, 0x28, &v); it->depth = v;
    it->world.x = rd_f32(d, len, 0x2C);
    it->world.y = rd_f32(d, len, 0x30);
    it->world.z = rd_f32(d, len, 0x34);
    it->world.plane = rd_f32(d, len, 0x3C);
    it->world.lift = rd_f32(d, len, 0x40);
    rd_i32(d, len, 0x44, &v); it->alpha = v > 255 ? 255 : (v < 0 ? 0 : v);
    it->world.scale_x = rd_f32(d, len, 0x48);
    it->world.scale_y = rd_f32(d, len, 0x4C);
    it->opacity = 0x100;        /* 0x0041B9C4, not from the record */
}

/*
 * The bitmap block, 0x0042E530. Only its NAME is wanted here: the original
 * reloads the image through 0x0042C110 - the same loader command 0x030 calls -
 * so a save carries the file name and not the pixels. The name is the first of
 * two NUL-terminated strings, and where they start is the one thing the block
 * version changes: 8 below version 2, 0x24 at version 2, and 0x28 from version
 * 3 up, because version 3 reads one more dword at 0x0042E599 after the
 * version-2 head at 0x0042E5A4 has already put the cursor at 0x24.
 */
static const char *bitmap_name(const uint8_t *d, int len)
{
    unsigned ver = 0;
    int at, i;
    if (!rd_u16(d, len, 0, &ver)) return NULL;
    at = ver >= 3 ? 0x28 : (ver == 2 ? 0x24 : 8);
    if (at >= len) return NULL;
    for (i = at; i < len; i++) if (!d[i]) return (const char *) d + at;
    return NULL;
}

/* One optional tagged block. Answers how many bytes it occupies, or 0 when
 * the next word is not this tag. */
static int block(const uint8_t *d, int len, int at, unsigned tag,
                 const uint8_t **payload, int *payload_len)
{
    unsigned t = 0;
    int32_t n = 0;
    if (!rd_u16(d, len, at, &t) || t != tag) return 0;
    if (!rd_i32(d, len, at + 2, &n) || n < 0 || at + 6 + n > len) return 0;
    *payload = d + at + 6;
    *payload_len = (int) n;
    return 6 + (int) n;
}

static int restore_into(cmvs_scene *s, cmvs_object *o, const uint8_t *d, int len)
{
    unsigned first = 0;
    unsigned ver = 0;
    int at = 0, step;
    int32_t shown = 0, ew = 0, eh = 0;
    const uint8_t *pay = NULL;
    int paylen = 0;

    if (!rd_u16(d, len, 0, &first)) return 0;
    if (first == 0xFFFF) {
        if (!rd_u16(d, len, 2, &ver)) return 0;
        at = 4;
    }
    if (at + 12 > len) return 0;
    rd_i32(d, len, at + 4, &shown);
    at += 12;
    if (ver >= 1) at += 12;
    if (ver >= 3) {
        if (!rd_i32(d, len, at, &ew) || !rd_i32(d, len, at + 4, &eh)) return 0;
        at += 8;
    }
    o->ew = ew;
    o->eh = eh;

    if ((step = block(d, len, at, 0x740, &pay, &paylen)) != 0) {
        const char *name = bitmap_name(pay, paylen);
        if (name && *name) {
            pb3_image img;
            char err[256];
            if (cmvs_game_image(s->game, name, &img, err, sizeof err)) {
                if (o->has_bitmap) pb3_free(&o->bitmap);
                o->bitmap = img;
                o->has_bitmap = 1;
            }
        }
        at += step;
    }
    /* +0xc14, +0xc30 and +0xc20: a second bitmap, and two objects this engine
     * has no model for. Stepped over by their own lengths, exactly as an
     * unknown command is stepped over by its argument count. */
    if ((step = block(d, len, at, 0x741, &pay, &paylen)) != 0) at += step;
    if ((step = block(d, len, at, 0x745, &pay, &paylen)) != 0) at += step;
    if ((step = block(d, len, at, 0x749, &pay, &paylen)) != 0) at += step;
    if ((step = block(d, len, at, 0x780, &pay, &paylen)) != 0) {
        restore_item(&o->item, pay, paylen);
        at += step;
    }
    o->item.visible = shown ? 1 : 0;
    /*
     * The eleven dwords at 0x004359B5. The original calls 0x00435260 with nine
     * of them when the first is non-zero; it is zero in every record of the
     * reference saves, so there is nothing here to apply and the tail is only
     * stepped over. A record that ever arrives with it set loses whatever
     * 0x00435260 would have done - that is *unproven*, not implemented.
     */
    if (ver >= 2) at += 0x2C;

    for (;;) {
        unsigned tag = 0, index = 0;
        int32_t n = 0;
        if (!rd_u16(d, len, at, &tag) || tag != 0x700) break;
        if (!rd_u16(d, len, at + 2, &index)) break;
        if (!rd_i32(d, len, at + 4, &n) || n < 0 || at + 8 + n > len) break;
        at += 8;
        if (index < (unsigned) CMVS_PARTS) {
            object_free(o->part[index]);
            o->part[index] = object_new();
            if (o->part[index]) restore_into(s, o->part[index], d + at, (int) n);
        }
        at += (int) n;
    }
    return 1;
}

int cmvs_scene_restore_text(cmvs_scene *s, int id, const uint8_t *data, int len)
{
    cmvs_text *t;
    unsigned ver = 0, count = 0;
    int at, i, name;
    int32_t w[39];

    if (!s || !data || len <= 0 || id < 0 || id >= CMVS_TEXT_IDS) return 0;
    if (!rd_u16(data, len, 0, &ver)) return 0;

    /* The record names its own object: an id no layer has registered is one
     * the script made with 0x100, and 0x0045D2D4 creates it here too. */
    t = cmvs_scene_text_by_id(s, id);
    if (!t) {
        if (!cmvs_scene_text_create(s, id)) return 0;
        t = cmvs_scene_text_by_id(s, id);
        if (!t) return 0;
    }

    /* u16 version, u32, then the font name, which this engine does not choose
     * per object - the session finds one face and cuts every glyph from it. */
    name = 6;
    while (name < len && data[name]) name++;
    if (name >= len) return 0;
    at = name + 1;
    if (ver >= 2) at += 8;
    for (i = 0; i < 39; i++)
        if (!rd_i32(data, len, at + i * 4, &w[i])) return 0;
    at += 0x9C + 0x94;
    if (!rd_u16(data, len, at, &count)) return 0;
    at += 2;

    /* Offset 0x04 + 4i of the original's object; the names are text.h's. */
    cmvs_text_clear(t);
    t->x = w[0];  t->y = w[1];
    t->rx = w[2]; t->ry = w[3]; t->rw = w[4]; t->rh = w[5];
    t->size = w[6]; t->gap = w[7]; t->lead = w[8];
    t->kinsoku = w[26] != 0;
    t->colour = (uint32_t) w[27];
    t->colour2 = (uint32_t) w[28];
    t->edge = (uint32_t) w[30];
    t->mode = w[32]; t->fade = w[33];
    t->speed = w[34];
    t->visible = w[35] != 0;
    t->pen_x = w[37]; t->pen_y = w[38];

    /*
     * The glyphs the line already holds, each the ten-dword descriptor
     * 0x004501B0 takes, behind the ordinal that routine writes back into it.
     * The descriptor is filled in 0x00450DC0 before the loop and per character
     * inside it, and that is where these names come from:
     *
     *   d0 high half  the cp932 character   (0x00450EB3)
     *   d2            the colour mode, +0x80 (0x00450E3A / 0x00450E43)
     *   d3 d4         the pen, +0x98 and +0x9c (0x00450EDE, 0x00450FA5)
     *   d5            the em size, +0x1c    (0x00450E00)
     *   d6 d7         the two colours, +0x70 and +0x74 (0x00450DFA)
     *   d8            +0x78, or the caller's own (0x00450E59)
     *   d9            the edge colour, +0x7c (0x00450DF7)
     *
     * d0's low half and d1 are *unproven*; nothing this engine draws with
     * reads them. They are on screen in the save, so they are on screen here:
     * the reveal clock is not restarted for them.
     */
    for (i = 0; i < (int) count; i++) {
        int32_t d[10];
        unsigned code;
        int j, ok = 1;
        for (j = 0; j < 10; j++)
            if (!rd_i32(data, len, at + 4 + j * 4, &d[j])) { ok = 0; break; }
        if (!ok) break;
        code = ((uint32_t) d[0] >> 16) & 0xFFFFu;
        if (code) {
            uint32_t was = t->colour, edge = t->edge;
            int size = t->size;
            t->colour = (uint32_t) d[6];
            t->edge = (uint32_t) d[9];
            t->size = d[5] > 0 ? d[5] : t->size;
            t->instant = 1;
            cmvs_text_append(t, code, d[3], d[4],
                             code > 0xFF ? t->size : t->size / 2);
            t->instant = 0;
            t->colour = was;
            t->edge = edge;
            t->size = size;
        }
        at += 0x2C;
    }
    return 1;
}

int cmvs_scene_restore_object(cmvs_scene *s, int object, const uint8_t *data, int len)
{
    if (!s || !data || len <= 0) return 0;
    if (object < 0 || object >= CMVS_OBJECTS + CMVS_LAYERS) return 0;
    object_free(s->object[object]);
    s->object[object] = object_new();
    if (!s->object[object]) return 0;
    if (restore_into(s, s->object[object], data, len)) return 1;
    object_free(s->object[object]);
    s->object[object] = NULL;
    return 0;
}

/*
 * A layer record, 0x0045D46B -> 0x00451B80. Its head is the text id and a
 * version of its own; then come the layer SETTINGS, which 0x00451BFD copies
 * raw into the layer object (0x270 dwords at version 3) and which are the
 * engine own in-memory layout rather than anything portable. This engine does
 * not have that layout and does not pretend to: the block is stepped over by
 * the length the original itself computes (0x9CC at 0x00451BFF, plus the four
 * bytes 0x00451CF0 adds), and what is applied is the graphic object behind it,
 * which is the same 0x00435710 record as a 0x700.
 */
int cmvs_scene_restore_layer(cmvs_scene *s, int layer, const uint8_t *data, int len,
                             int *text_id)
{
    unsigned id = 0, ver = 0;
    int at;
    if (!s || !data || len <= 0) return 0;
    if (layer < 0 || layer >= CMVS_LAYERS) return 0;
    if (!rd_u16(data, len, 0, &id) || !rd_u16(data, len, 2, &ver)) return 0;
    if (text_id) *text_id = (int) id;
    /* 0x00451BD3: version 1 and anything else leaves the cursor at 4, versions
     * 2 and 3 at 0x0c; only version 3 carries the settings block. */
    if (ver != 3) return 0;
    at = 0x9CC + 4;
    if (at >= len) return 0;
    return cmvs_scene_restore_object(s, CMVS_LAYER_OBJECT(layer), data + at, len - at);
}

/* ------------------------------------------------------------------ blit */

/*
 * `opaque` is the image's own has_alpha, inverted: a PB3 with three channels
 * leaves the alpha byte at zero for every pixel, so reading it would make a
 * background invisible. bg990a.pb3, the first thing ChronoClock's played scene
 * puts on the screen, is exactly that image.
 */
static void blend(uint8_t *dst, const uint8_t *src, int alpha, int opaque)
{
    int a = (opaque ? 255 : src[3]) * alpha / 255;
    if (a <= 0) return;
    if (a >= 255) { memcpy(dst, src, 4); dst[3] = 0xFF; return; }
    dst[0] = (uint8_t) ((src[0] * a + dst[0] * (255 - a)) / 255);
    dst[1] = (uint8_t) ((src[1] * a + dst[1] * (255 - a)) / 255);
    dst[2] = (uint8_t) ((src[2] * a + dst[2] * (255 - a)) / 255);
    dst[3] = 0xFF;
}

/*
 * Which camera places a staged item: 0x00435f7e reads camera 0 for kind 2
 * (0x00418ab0, world+8) and camera 1 for kind 3 (0x00416ed0, world+0xc).
 */
static const cmvs_camera *camera_of(const cmvs_scene *s, const cmvs_item *it)
{
    return &s->camera[it->kind == 3 ? 1 : 0];
}

/* Answers where the camera puts this item, or 0 when the item is a flat one
 * or the camera refuses it. */
static int staged(const cmvs_scene *s, const cmvs_item *it, cmvs_projection *at)
{
    if (it->kind != 2 && it->kind != 3) return 0;
    return cmvs_camera_project(camera_of(s, it), &it->world, at);
}

/*
 * One item, at a place and a scale its caller has already worked out. The
 * loop walks the DESTINATION so a scaled item costs what it covers rather
 * than what it came from; at scale 1 the source and destination steps are the
 * same pixel, so a staged scene composed at its own depth is byte for byte
 * what the flat blit used to produce.
 */
static void draw_item(cmvs_scene *s, const cmvs_object *o,
                      const pb3_image *from, float dx, float dy,
                      float scale_x, float scale_y, int alpha)
{
    const cmvs_item *it = &o->item;
    int sw = it->sw, sh = it->sh, sx = it->sx, sy = it->sy;
    int dw, dh, row, col, ix, iy;

    if (!it->used || !it->visible || !from || !from->pixels) return;
    if (alpha <= 0) return;
    /*
     * A source rectangle of nothing means the WHOLE bitmap. 0x0041b780 zeroes
     * the draw item and command 0x044 writes exactly what the script gives it
     * (0x0041bd00 is four stores and no defaulting), so snky01.ps3's
     * background - object 29, bitmap bg990a.pb3, rectangle (0, 0, 0, 0),
     * position (0, 0) - carries no size at all and would draw nothing if a
     * zero rectangle were taken literally. The bitmap's own extent is what the
     * engine falls back to; it is the only reading under which the scene has a
     * background.
     */
    if (sw <= 0) sw = from->width - sx;
    if (sh <= 0) sh = from->height - sy;
    if (sw <= 0 || sh <= 0) return;
    if (scale_x <= 0.0f || scale_y <= 0.0f) return;

    dw = (int) (sw * scale_x + 0.5f);
    dh = (int) (sh * scale_y + 0.5f);
    if (dw <= 0 || dh <= 0) return;
    ix = (int) (dx < 0.0f ? dx - 0.5f : dx + 0.5f);
    iy = (int) (dy < 0.0f ? dy - 0.5f : dy + 0.5f);

    for (row = 0; row < dh; row++) {
        int dr = iy + row;
        int sr = sy + (int) ((row + 0.5f) / scale_y);
        if (dr < 0 || dr >= s->height) continue;
        if (sr < 0 || sr >= from->height) continue;
        for (col = 0; col < dw; col++) {
            int dc = ix + col;
            int sc = sx + (int) ((col + 0.5f) / scale_x);
            if (dc < 0 || dc >= s->width) continue;
            if (sc < 0 || sc >= from->width) continue;
            blend(s->frame + 4 * ((size_t) dr * s->width + dc),
                  from->pixels + 4 * ((size_t) sr * from->width + sc),
                  alpha, !from->has_alpha);
        }
    }
    s->drawn++;
}

/*
 * An object draws itself and then its parts, in index order; a part with no
 * bitmap of its own draws out of the nearest one above it, which is what makes
 * a sheet of buttons work.
 *
 * A part's position is its PARENT'S, plus its own. In the engine the parts are
 * composed into the parent object's own surface - the one 0x17c gives an extent
 * - and that surface is then drawn where the parent sits, so a part never knows
 * where on the screen it ends up. The typed line is what shows it: snky01.ps3
 * lays its glyph sprites out at x = 0, 30, 60, ... and y = 0, and they belong
 * inside a message window whose object sits at y = 540.
 *
 * A STAGED item ignores all of that. Its place is not its own to give: the
 * camera puts its anchor somewhere on the screen and the bitmap hangs off that
 * point, which is why ChronoClock's rooftop background - anchored at its own
 * centre and never given a position - belongs in the middle of the frame and
 * not at (-910, -512).
 */
static void draw_object(cmvs_scene *s, const cmvs_object *o,
                        const pb3_image *inherited, float ox, float oy,
                        float scale_x, float scale_y, int alpha)
{
    const pb3_image *from = o->has_bitmap ? &o->bitmap : inherited;
    const cmvs_item *it = &o->item;
    float dx, dy, sx = scale_x, sy = scale_y;
    int i, a = alpha * it->alpha / 255;
    cmvs_projection at;

    if (!o) return;
    if (staged(s, it, &at)) {
        sx = at.scale_x;
        sy = at.scale_y;
        dx = at.x - it->ox * sx;
        dy = at.y - it->oy * sy;
        a = a * at.alpha / 255;
        draw_item(s, o, from, dx, dy, sx, sy, a);
        for (i = 0; i < CMVS_PARTS; i++)
            if (o->part[i]) draw_object(s, o->part[i], from, dx, dy, sx, sy, a);
        return;
    }
    dx = ox + (it->x - it->ox) * sx;
    dy = oy + (it->y - it->oy) * sy;
    draw_item(s, o, from, dx, dy, sx, sy, a);
    ox += (it->x + it->ox) * sx;
    oy += (it->y + it->oy) * sy;
    for (i = 0; i < CMVS_PARTS; i++)
        if (o->part[i]) draw_object(s, o->part[i], from, ox, oy, sx, sy, a);
}

/*
 * The pass an item belongs to. 0x00435e7f and 0x00435f7e turn the item's first
 * field into an index into the compositor's list array: kind 1 goes to 0, kind
 * 2 to 2, kind 3 to 6 and everything else to 3, and the lists are drawn in
 * that order. It is what keeps a staged background behind the flat interface
 * drawn over it without either of them naming the other.
 */
static int pass_of(const cmvs_item *it)
{
    if (it->kind == 1) return 0;
    if (it->kind == 2) return 2;
    if (it->kind == 3) return 6;
    return 3;
}

static void compose_flat_pass(cmvs_scene *s, int pass)
{
    int i, order = 0, any = 0;
    /*
     * They go down in the order command 0x047 gives them (item +0x28), not in
     * object number, and the list is kept DESCENDING (0x00435bc0): a bigger
     * number is FARTHER, so it is drawn first and everything smaller lands on
     * top of it. The title screen is the proof - the park is object 10's own
     * item at 32 and the four captions are its parts at 28 - and so is the
     * load window, which intproc.ps3 builds at 3..6 while the title is still
     * on screen and which the park painted over when this ran the other way.
     * A scene's own background is NOT ordered here: snky01.ps3 stages both its
     * background and its character, and a staged pass is sorted by the
     * distance in front of the camera instead. Equal orders keep their object
     * number as the
     * tie-break, which is what the title screen relies on.
     */
    for (i = 0; i < CMVS_OBJECTS; i++) {
        if (!s->object[i] || pass_of(&s->object[i]->item) != pass) continue;
        if (!any || s->object[i]->item.depth > order) order = s->object[i]->item.depth;
        any = 1;
    }
    while (any) {
        int next = 0, more = 0;
        for (i = 0; i < CMVS_OBJECTS; i++)
            if (s->object[i] && pass_of(&s->object[i]->item) == pass
                && s->object[i]->item.depth == order)
                draw_object(s, s->object[i], NULL, 0.0f, 0.0f, 1.0f, 1.0f, 255);
        for (i = 0; i < CMVS_OBJECTS; i++) {
            if (!s->object[i] || pass_of(&s->object[i]->item) != pass) continue;
            if (s->object[i]->item.depth >= order) continue;
            if (!more || s->object[i]->item.depth > next) next = s->object[i]->item.depth;
            more = 1;
        }
        if (!more) break;
        order = next;
    }
}

/*
 * A staged pass goes far to near. 0x00435bc0 keeps its list in descending key
 * order and the key for a staged item is its distance in front of the camera
 * (0x0041bc80 plus the camera's own z at 0x00443d80), so the far background
 * lands before the character standing in front of it - the same answer command
 * 0x047 gives a flat scene, taken from the world instead of from a number.
 */
static void compose_staged_pass(cmvs_scene *s, int pass)
{
    int i, drawn[CMVS_OBJECTS], count = 0, done = 0;
    for (i = 0; i < CMVS_OBJECTS; i++) {
        if (!s->object[i] || pass_of(&s->object[i]->item) != pass) continue;
        drawn[count++] = i;
    }
    while (done < count) {
        int best = -1;
        float far = 0.0f;
        for (i = 0; i < count; i++) {
            const cmvs_object *o;
            float d;
            if (drawn[i] < 0) continue;
            o = s->object[drawn[i]];
            d = o->item.world.z - camera_of(s, &o->item)->z;
            if (best < 0 || d > far) { best = i; far = d; }
        }
        if (best < 0) break;
        draw_object(s, s->object[drawn[best]], NULL, 0.0f, 0.0f, 1.0f, 1.0f, 255);
        drawn[best] = -1;
        done++;
    }
}

const uint8_t *cmvs_scene_compose(cmvs_scene *s, int *width, int *height)
{
    static const int order[] = { 0, 2, 3, 6 };
    size_t p;
    int i;
    if (!s) return NULL;
    memset(s->frame, 0, (size_t) s->width * s->height * 4);
    s->drawn = 0;
    /*
     * The graphic objects go down first and the display layers over them.
     * snky01.ps3 is what settles it: the scene's BACKGROUND is graphic object
     * 29 (bg990a.pb3, the summer sky, put there by procedure 33) and the
     * message window with its line is display layer 0, so a reader sees the
     * window over the sky and not the sky over the window. The title screen,
     * which is graphic objects alone, is unaffected either way.
     */
    for (p = 0; p < sizeof order / sizeof *order; p++) {
        if (order[p] == 2 || order[p] == 6) compose_staged_pass(s, order[p]);
        else compose_flat_pass(s, order[p]);
    }
    for (i = CMVS_OBJECTS + CMVS_LAYERS - 1; i >= CMVS_OBJECTS; i--) {
        int layer = i - CMVS_OBJECTS;
        const cmvs_object *o = s->object[i];
        if (o) draw_object(s, o, NULL, 0.0f, 0.0f, 1.0f, 1.0f, 255);
        /* The line goes over the window it is written in, and the window is
         * the layer's own object: the message box sits at y = 540 and the pen
         * counts from there. */
        cmvs_text_compose(&s->text[layer], s->font, s->frame, s->width, s->height,
                          o ? o->item.x + o->item.ox : 0,
                          o ? o->item.y + o->item.oy : 0);
    }
    /*
     * The text objects a script made for itself go last, at their own
     * position: a choice caption is written after the bar it sits on and the
     * original's own object for it is created then too.
     */
    for (i = 0; i < CMVS_TEXT_IDS; i++) {
        if (!s->id_used[i]) continue;
        cmvs_text_compose(&s->id_text[i], s->font, s->frame, s->width, s->height,
                          s->id_text[i].x, s->id_text[i].y);
    }
    if (width) *width = s->width;
    if (height) *height = s->height;
    return s->frame;
}

/* ------------------------------------------------------- showing a layer */

/*
 * Two flags and they are not the same one. 0x00452550 (command 0x14e) is the
 * script SHOWING the layer: it writes +0x04 and clears +0x08. 0x00452690
 * (command 0x14c) is the player HIDING it - the message window the toolbar
 * and KEY_FUNCTION_07 take away - and it refuses outright while +0x04 is
 * clear, because there is nothing to take away. Both end by showing or hiding
 * the layer's own graphic object and its text object together.
 */
void cmvs_layer_present(cmvs_scene *s, int layer, int shown)
{
    cmvs_layer *l;
    if (!s || layer < 0 || layer >= CMVS_LAYERS) return;
    l = &s->layer[layer];
    shown = shown ? 1 : 0;
    /*
     * 0x00466d61: the command compares the flag with what the layer already
     * carries and returns without touching anything when they are the same.
     * That is not a saving, it is the difference between a script that says
     * "show the message window" on every line and a player who has just taken
     * the window away with the toolbar: the second flag survives.
     */
    if (l->shown == shown) return;
    l->shown = shown;
    l->hidden = 0;
    cmvs_text_show(&s->text[layer], l->shown);
    cmvs_scene_show(s, CMVS_LAYER_OBJECT(layer), -1, l->shown);
    /* and a layer that has just been shown is put back at its own origin
     * (0x00452587 -> 0x004503e0), which is where its text is drawn from. */
    if (l->shown) cmvs_text_at(&s->text[layer], l->x, l->y);
}

void cmvs_layer_hide(cmvs_scene *s, int layer, int hidden)
{
    cmvs_layer *l;
    if (!s || layer < 0 || layer >= CMVS_LAYERS) return;
    l = &s->layer[layer];
    if (!l->shown) return;          /* 0x00452696 */
    l->hidden = hidden ? 1 : 0;
    cmvs_text_show(&s->text[layer], !l->hidden);
    cmvs_scene_show(s, CMVS_LAYER_OBJECT(layer), -1, !l->hidden);
}

int cmvs_layer_hidden(const cmvs_scene *s, int layer)
{
    if (!s || layer < 0 || layer >= CMVS_LAYERS) return 0;
    return s->layer[layer].hidden;
}

/* ------------------------------------------------- the layer's own sprites */

static cmvs_sprite *sprite_of(cmvs_scene *s, int layer, int i)
{
    /* The two bounds every one of these commands checks before it touches
     * anything: 0x004686b3 for the layer and 0x004686c3 for the sprite. */
    if (!s || layer < 0 || layer >= CMVS_LAYERS) return NULL;
    if (i < 0 || i >= CMVS_LAYER_SPRITES) return NULL;
    return &s->layer[layer].sprite[i];
}

void cmvs_layer_sprite_state(cmvs_scene *s, int layer, int sprite, int state,
                             int sx, int sy, int sw, int sh, int ox, int oy)
{
    cmvs_sprite *e = sprite_of(s, layer, sprite);
    if (!e || state < 0 || state >= CMVS_SPRITE_STATES) return;
    e->sx[state] = (int16_t) sx;   /* 0x00452960 stores every one as a word */
    e->sy[state] = (int16_t) sy;
    e->sw[state] = (int16_t) sw;
    e->sh[state] = (int16_t) sh;
    e->ox[state] = (int16_t) ox;
    e->oy[state] = (int16_t) oy;
}

void cmvs_layer_sprite_rect(cmvs_scene *s, int layer, int sprite,
                            int x, int y, int w, int h)
{
    cmvs_sprite *e = sprite_of(s, layer, sprite);
    if (!e) return;
    /* 0x004529c0 keeps the rectangle as four UNSIGNED words, which is why a
     * sprite is never placed left of or above its layer's own origin. */
    e->x = (uint16_t) x;
    e->y = (uint16_t) y;
    e->w = (uint16_t) w;
    e->h = (uint16_t) h;
}

void cmvs_layer_sprite_show(cmvs_scene *s, int layer, int sprite, int on)
{
    cmvs_sprite *e = sprite_of(s, layer, sprite);
    if (!e) return;
    e->on = on ? 1 : 0;
    /* and the part that draws it goes with it: 0x00452a00 asks the layer's own
     * object for part (i + 0x10) and calls the same 0x00432b70 command 0x215
     * uses. A layer whose object is not there yet simply has nothing to show. */
    cmvs_scene_show(s, CMVS_LAYER_OBJECT(layer), CMVS_LAYER_PART(sprite), e->on);
}

int cmvs_layer_sprite_on(const cmvs_scene *s, int layer, int sprite)
{
    const cmvs_sprite *e = sprite_of((cmvs_scene *) s, layer, sprite);
    return e ? (e->on != 0) : 0;
}

int cmvs_layer_sprite_read(const cmvs_scene *s, int layer, int sprite,
                           int *x, int *y, int *w, int *h, int *worn)
{
    const cmvs_sprite *e = sprite_of((cmvs_scene *) s, layer, sprite);
    if (!e || !e->on) return 0;
    if (x) *x = s->layer[layer].x + e->x;
    if (y) *y = s->layer[layer].y + e->y;
    if (w) *w = e->w;
    if (h) *h = e->h;
    if (worn) *worn = e->worn;
    return 1;
}

void cmvs_layer_sprite_wear(cmvs_scene *s, int layer, int sprite, int state)
{
    cmvs_sprite *e = sprite_of(s, layer, sprite);
    if (!e || state < 0 || state >= CMVS_SPRITE_STATES) return;
    /* 0x004688d0 compares the state with the one already worn FIRST and
     * returns without touching the part when they are the same. */
    if (e->worn == state) return;
    e->worn = state;
    if (cmvs_scene_exists(s, CMVS_LAYER_OBJECT(layer), CMVS_LAYER_PART(sprite)))
        cmvs_scene_source(s, CMVS_LAYER_OBJECT(layer), CMVS_LAYER_PART(sprite),
                          e->sx[state], e->sy[state], e->sw[state], e->sh[state]);
}

int cmvs_layer_sprite_worn(const cmvs_scene *s, int layer, int sprite)
{
    const cmvs_sprite *e = sprite_of((cmvs_scene *) s, layer, sprite);
    return e ? e->worn : 0;
}

void cmvs_layer_move(cmvs_scene *s, int layer, int mode, int x, int y)
{
    cmvs_layer *l;
    if (!s || layer < 0 || layer >= CMVS_LAYERS) return;
    l = &s->layer[layer];
    if (mode == 0) { l->x = x; l->y = y; }
    else if (mode == 1) { l->x += x; l->y += y; }
    else { l->x = x; l->y = x; }   /* 0x00452776, the fall-through */
    /* and the layer's object moves with it (0x0045279a -> 0x00433340 ->
     * 0x0041bd60, which is the same item position command 0x045 writes). */
    cmvs_scene_at(s, CMVS_LAYER_OBJECT(layer), -1, l->x, l->y);
    cmvs_text_at(&s->text[layer], l->x, l->y);
}

void cmvs_layer_origin(const cmvs_scene *s, int layer, int *x, int *y)
{
    if (x) *x = 0;
    if (y) *y = 0;
    if (!s || layer < 0 || layer >= CMVS_LAYERS) return;
    if (x) *x = s->layer[layer].x;
    if (y) *y = s->layer[layer].y;
}

int cmvs_layer_hit(cmvs_scene *s, int layer, cmvs_input *in,
                   int *hover, int *click, int *held)
{
    cmvs_layer *l;
    int i, found = 0, down = 0;
    if (hover) *hover = -1;      /* 0x00452a96, and it is -1 rather than 0 */
    if (click) *click = 0;
    if (held) *held = 0;
    if (!s || !in || layer < 0 || layer >= CMVS_LAYERS) return 0;
    l = &s->layer[layer];
    /* A press starts by forgetting where the last one began (0x00452aba), so
     * that only a sprite the loop below finds can claim it. */
    if (cmvs_input_pressed(in, CMVS_FN_CONFIRM)) l->press_from = -1;
    for (i = 0; i < CMVS_LAYER_SPRITES; i++) {
        const cmvs_sprite *e = &l->sprite[i];
        int left, top, right, bottom, over = 0;
        if (!e->on) continue;
        left = l->x + e->x;
        top = l->y + e->y;
        right = left + e->w;
        bottom = top + e->h;
        /* 0x00452b33: the four tests are inclusive on both edges. */
        if (left > in->x || top > in->y || right < in->x || bottom < in->y) continue;
        if (cmvs_input_pressed(in, CMVS_FN_CONFIRM)) l->press_from = i;
        if (hover) *hover = i;
        if (cmvs_input_released(in, CMVS_FN_CONFIRM, &over)) {
            cmvs_input_clear(in, CMVS_FN_CONFIRM);          /* 0x00452b76 */
            if (l->press_from == i && click) *click = 1;
        }
        if (over && held) *held = 1;
        found = 1;
    }
    /* After the table: a release anywhere ends a press that began on a sprite,
     * and the button coming up forgets it either way (0x00452bc3 onwards). */
    if (cmvs_input_released(in, CMVS_FN_CONFIRM, NULL) && l->press_from >= 0) {
        cmvs_input_clear(in, CMVS_FN_CONFIRM);
        l->press_from = -1;
    }
    cmvs_input_released(in, CMVS_FN_CONFIRM, &down);
    if (!down) l->press_from = -1;
    return found;
}

int cmvs_layer_hit_rect(const cmvs_scene *s, int layer, const cmvs_input *in,
                        int x, int y, int w, int h)
{
    int left, top;
    if (!s || !in || layer < 0 || layer >= CMVS_LAYERS) return 0;
    left = s->layer[layer].x + x;
    top = s->layer[layer].y + y;
    /* 0x00452804: the near edges are inclusive and the far ones are not, which
     * is one pixel tighter than 0x00452a80's table above. */
    if (left > in->x || top > in->y) return 0;
    if (left + w <= in->x || top + h <= in->y) return 0;
    return 1;
}

int cmvs_scene_drawn(const cmvs_scene *s) { return s ? s->drawn : 0; }

/*
 * Command 0x18d's half of the draw item (0x00467be0). The id is the layer
 * sprite convention 0x00451d30 uses: below zero is the layer's own object and
 * anything else is that part of it.
 */
void cmvs_scene_item_read(const cmvs_scene *s, int object, int part,
                          int *x, int *y, int *depth, int *alpha,
                          float *scale_x, float *scale_y)
{
    const cmvs_object *o = reach((cmvs_scene *) s, object, part);
    if (!o) return;
    if (x) *x = o->item.x;
    if (y) *y = o->item.y;
    if (depth) *depth = o->item.depth;
    if (alpha) *alpha = o->item.alpha;
    if (scale_x) *scale_x = o->item.world.scale_x;
    if (scale_y) *scale_y = o->item.world.scale_y;
}
