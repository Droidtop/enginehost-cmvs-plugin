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
} cmvs_item;

typedef struct cmvs_object {
    cmvs_item item;
    pb3_image bitmap;
    int has_bitmap;
    int ew, eh;                  /* +0xc44, +0xc48: the object's own extent */
    struct cmvs_object **part;   /* CMVS_PARTS pointers, allocated with the object */
} cmvs_object;

struct cmvs_scene {
    cmvs_game *game;
    int width, height;
    uint8_t *frame;              /* BGRA, 4 * width * height */
    /* The 256 graphic objects, then the 8 layer objects: one table, because
     * 0x00451d30 reaches a layer's sprites with the object family's own
     * accessor and there is nothing to tell apart below that call. */
    cmvs_object *object[CMVS_OBJECTS + CMVS_LAYERS];
    cmvs_text text[CMVS_LAYERS];
    int of_id[CMVS_TEXT_IDS];    /* the table at +0xbb0: which layer's text */
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
    return s;
}

void cmvs_scene_free(cmvs_scene *s)
{
    int i;
    if (!s) return;
    for (i = 0; i < CMVS_OBJECTS + CMVS_LAYERS; i++) object_free(s->object[i]);
    for (i = 0; i < CMVS_LAYERS; i++) cmvs_text_free(&s->text[i]);
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

cmvs_text *cmvs_scene_text(cmvs_scene *s, int layer)
{
    if (!s || layer < 0 || layer >= CMVS_LAYERS) return NULL;
    return &s->text[layer];
}

void cmvs_scene_text_register(cmvs_scene *s, int id, int layer)
{
    if (!s || id < 0 || id >= CMVS_TEXT_IDS) return;
    s->of_id[id] = (layer >= 0 && layer < CMVS_LAYERS) ? layer : -1;
}

void cmvs_scene_text_tick(cmvs_scene *s, int ms)
{
    int i;
    if (!s) return;
    for (i = 0; i < CMVS_LAYERS; i++) cmvs_text_tick(&s->text[i], ms);
}

cmvs_text *cmvs_scene_text_by_id(cmvs_scene *s, int id)
{
    if (!s || id < 0 || id >= CMVS_TEXT_IDS || s->of_id[id] < 0) return NULL;
    return &s->text[s->of_id[id]];
}

void cmvs_scene_font(cmvs_scene *s, cmvs_font *font) { if (s) s->font = font; }

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

static void draw_item(cmvs_scene *s, const cmvs_object *o,
                      const pb3_image *from, int ox, int oy)
{
    const cmvs_item *it = &o->item;
    int sw = it->sw, sh = it->sh, sx = it->sx, sy = it->sy;
    int dx = ox + it->x - it->ox, dy = oy + it->y - it->oy;
    int row, col;

    if (!it->used || !it->visible || !from || !from->pixels) return;
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

    for (row = 0; row < sh; row++) {
        int sr = sy + row, dr = dy + row;
        if (sr < 0 || sr >= from->height || dr < 0 || dr >= s->height) continue;
        for (col = 0; col < sw; col++) {
            int sc = sx + col, dc = dx + col;
            if (sc < 0 || sc >= from->width || dc < 0 || dc >= s->width) continue;
            blend(s->frame + 4 * ((size_t) dr * s->width + dc),
                  from->pixels + 4 * ((size_t) sr * from->width + sc),
                  it->alpha, !from->has_alpha);
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
 */
static void draw_object(cmvs_scene *s, const cmvs_object *o,
                        const pb3_image *inherited, int ox, int oy)
{
    const pb3_image *from = o->has_bitmap ? &o->bitmap : inherited;
    int i;
    if (!o) return;
    draw_item(s, o, from, ox, oy);
    ox += o->item.x + o->item.ox;
    oy += o->item.y + o->item.oy;
    for (i = 0; i < CMVS_PARTS; i++)
        if (o->part[i]) draw_object(s, o->part[i], from, ox, oy);
}

const uint8_t *cmvs_scene_compose(cmvs_scene *s, int *width, int *height)
{
    int i, order, any = 0;
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
    /*
     * And they go down in the order command 0x047 gives them (item +0x28),
     * not in object number: snky01.ps3 puts its background at 32 and the
     * character sprite that stands in front of it at 99, while the sprite is
     * object 20 and the background object 29. By number the background wins
     * and the girl is behind her own scenery. Equal orders keep their object
     * number as the tie-break, which is what the title screen relies on.
     */
    order = 0;
    for (i = 0; i < CMVS_OBJECTS; i++) {
        if (!s->object[i]) continue;
        if (!any || s->object[i]->item.depth < order) order = s->object[i]->item.depth;
        any = 1;
    }
    while (any) {
        int next = 0, more = 0;
        for (i = 0; i < CMVS_OBJECTS; i++)
            if (s->object[i] && s->object[i]->item.depth == order)
                draw_object(s, s->object[i], NULL, 0, 0);
        for (i = 0; i < CMVS_OBJECTS; i++) {
            if (!s->object[i] || s->object[i]->item.depth <= order) continue;
            if (!more || s->object[i]->item.depth < next) next = s->object[i]->item.depth;
            more = 1;
        }
        if (!more) break;
        order = next;
    }
    for (i = CMVS_OBJECTS + CMVS_LAYERS - 1; i >= CMVS_OBJECTS; i--) {
        int layer = i - CMVS_OBJECTS;
        const cmvs_object *o = s->object[i];
        if (o) draw_object(s, o, NULL, 0, 0);
        /* The line goes over the window it is written in, and the window is
         * the layer's own object: the message box sits at y = 540 and the pen
         * counts from there. */
        cmvs_text_compose(&s->text[layer], s->font, s->frame, s->width, s->height,
                          o ? o->item.x + o->item.ox : 0,
                          o ? o->item.y + o->item.oy : 0);
    }
    if (width) *width = s->width;
    if (height) *height = s->height;
    return s->frame;
}

int cmvs_scene_drawn(const cmvs_scene *s) { return s ? s->drawn : 0; }
