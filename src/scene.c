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
    int mode;
    int sx, sy, sw, sh;
    int ox, oy;
    int x, y;
    int size;
    int alpha;
    int opacity;
} cmvs_item;

typedef struct cmvs_object {
    cmvs_item item;
    pb3_image bitmap;
    int has_bitmap;
    struct cmvs_object **part;   /* CMVS_PARTS pointers, allocated with the object */
} cmvs_object;

struct cmvs_scene {
    cmvs_game *game;
    int width, height;
    uint8_t *frame;              /* BGRA, 4 * width * height */
    cmvs_object *object[CMVS_OBJECTS];
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
    if (width <= 0 || height <= 0) return NULL;
    s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->game = game;
    s->width = width;
    s->height = height;
    s->frame = calloc((size_t) width * height, 4);
    if (!s->frame) { free(s); return NULL; }
    return s;
}

void cmvs_scene_free(cmvs_scene *s)
{
    int i;
    if (!s) return;
    for (i = 0; i < CMVS_OBJECTS; i++) object_free(s->object[i]);
    free(s->frame);
    free(s);
}

/* The object a command means, with -1 for "the object itself" - the same test
 * every one of the setters makes before it calls 0x00432bd0. */
static cmvs_object *reach(cmvs_scene *s, int object, int part)
{
    cmvs_object *o;
    if (object < 0 || object >= CMVS_OBJECTS) return NULL;
    o = s->object[object];
    if (!o || part < 0) return o;
    if (part >= CMVS_PARTS) return NULL;
    return o->part[part];
}

int cmvs_scene_object(cmvs_scene *s, int object)
{
    if (object < 0 || object >= CMVS_OBJECTS) return 0;
    object_free(s->object[object]);
    s->object[object] = object_new();
    return s->object[object] != NULL;
}

int cmvs_scene_part(cmvs_scene *s, int object, int part)
{
    cmvs_object *o;
    if (object < 0 || object >= CMVS_OBJECTS || part < 0 || part >= CMVS_PARTS) return 0;
    o = s->object[object];
    if (!o) return 0;
    object_free(o->part[part]);
    o->part[part] = object_new();
    return o->part[part] != NULL;
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

int cmvs_scene_item(cmvs_scene *s, int object, int part)
{
    cmvs_object *o = reach(s, object, part);
    if (!o) return 0;
    memset(&o->item, 0, sizeof o->item);
    o->item.used = 1;
    o->item.mode = 1;
    o->item.alpha = 0xFF;
    o->item.opacity = 0x100;
    return 1;
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

void cmvs_scene_size(cmvs_scene *s, int object, int part, int size)
{
    cmvs_object *o = reach(s, object, part);
    if (!o) return;
    o->item.size = size;
}

/* ------------------------------------------------------------------ blit */

static void blend(uint8_t *dst, const uint8_t *src, int alpha)
{
    int a = src[3] * alpha / 255;
    if (a <= 0) return;
    if (a >= 255) { memcpy(dst, src, 4); dst[3] = 0xFF; return; }
    dst[0] = (uint8_t) ((src[0] * a + dst[0] * (255 - a)) / 255);
    dst[1] = (uint8_t) ((src[1] * a + dst[1] * (255 - a)) / 255);
    dst[2] = (uint8_t) ((src[2] * a + dst[2] * (255 - a)) / 255);
    dst[3] = 0xFF;
}

static void draw_item(cmvs_scene *s, const cmvs_object *o, const pb3_image *from)
{
    const cmvs_item *it = &o->item;
    int sw = it->sw, sh = it->sh, sx = it->sx, sy = it->sy;
    int dx = it->x + it->ox, dy = it->y + it->oy;
    int row, col;

    if (!it->used || !from || !from->pixels) return;
    if (sw <= 0 || sh <= 0) return;

    for (row = 0; row < sh; row++) {
        int sr = sy + row, dr = dy + row;
        if (sr < 0 || sr >= from->height || dr < 0 || dr >= s->height) continue;
        for (col = 0; col < sw; col++) {
            int sc = sx + col, dc = dx + col;
            if (sc < 0 || sc >= from->width || dc < 0 || dc >= s->width) continue;
            blend(s->frame + 4 * ((size_t) dr * s->width + dc),
                  from->pixels + 4 * ((size_t) sr * from->width + sc),
                  it->alpha);
        }
    }
    s->drawn++;
}

/* An object draws itself and then its parts, in index order; a part with no
 * bitmap of its own draws out of the nearest one above it, which is what makes
 * a sheet of buttons work. */
static void draw_object(cmvs_scene *s, const cmvs_object *o, const pb3_image *inherited)
{
    const pb3_image *from = o->has_bitmap ? &o->bitmap : inherited;
    int i;
    if (!o) return;
    draw_item(s, o, from);
    for (i = 0; i < CMVS_PARTS; i++)
        if (o->part[i]) draw_object(s, o->part[i], from);
}

const uint8_t *cmvs_scene_compose(cmvs_scene *s, int *width, int *height)
{
    int i;
    if (!s) return NULL;
    memset(s->frame, 0, (size_t) s->width * s->height * 4);
    s->drawn = 0;
    for (i = 0; i < CMVS_OBJECTS; i++)
        if (s->object[i]) draw_object(s, s->object[i], NULL);
    if (width) *width = s->width;
    if (height) *height = s->height;
    return s->frame;
}

int cmvs_scene_drawn(const cmvs_scene *s) { return s ? s->drawn : 0; }
