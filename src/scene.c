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

struct cmvs_scene {
    cmvs_game *game;
    int width, height;
    uint8_t *frame;              /* BGRA, 4 * width * height */
    /* The 256 graphic objects, then the 8 layer objects: one table, because
     * 0x00451d30 reaches a layer's sprites with the object family's own
     * accessor and there is nothing to tell apart below that call. */
    cmvs_object *object[CMVS_OBJECTS + CMVS_LAYERS];
    cmvs_camera camera[CMVS_CAMERAS];
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
     * object number: snky01.ps3 puts its background at 32 and the character
     * sprite that stands in front of it at 99, while the sprite is object 20
     * and the background object 29. By number the background wins and the girl
     * is behind her own scenery. Equal orders keep their object number as the
     * tie-break, which is what the title screen relies on.
     */
    for (i = 0; i < CMVS_OBJECTS; i++) {
        if (!s->object[i] || pass_of(&s->object[i]->item) != pass) continue;
        if (!any || s->object[i]->item.depth < order) order = s->object[i]->item.depth;
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
            if (s->object[i]->item.depth <= order) continue;
            if (!more || s->object[i]->item.depth < next) next = s->object[i]->item.depth;
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
    if (width) *width = s->width;
    if (height) *height = s->height;
    return s->frame;
}

int cmvs_scene_drawn(const cmvs_scene *s) { return s ? s->drawn : 0; }
