#include "menu.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    int part;
    int sx, sy, sw, sh;
    int ox, oy;
} cmvs_menu_sprite;

typedef struct cmvs_menu_item {
    struct cmvs_menu_item *next;
    int enabled;
    int id;
    int x, y, w, h;
    int prev_id, next_id;
    cmvs_menu_sprite state[CMVS_MENU_STATES];
} cmvs_menu_item;

typedef struct {
    int used;
    int object;                  /* +0x24: the graphic object the parts live on */
    cmvs_menu_item *head;        /* +0x00 */
    cmvs_menu_item *current;     /* +0x08: what the pointer is on */
    cmvs_menu_item *press_from;  /* +0x30: where the button went down */
    int pressed;                 /* +0x2c: the pressed sprite is showing */
} cmvs_menu;

struct cmvs_menus {
    cmvs_menu menu[CMVS_MENUS];
};

/*
 * The menu also remembers the window size and the game's own screen size
 * (+0x0c/+0x10 and +0x1c/+0x20) and scales the pointer between them on every
 * poll. Neither is kept here: the pointer reaches this file already in engine
 * coordinates, converted by whichever frontend owns the display.
 */

cmvs_menus *cmvs_menus_new(void)
{
    return calloc(1, sizeof(cmvs_menus));
}

static void items_free(cmvs_menu *m)
{
    cmvs_menu_item *it = m->head;
    while (it) {
        cmvs_menu_item *next = it->next;
        free(it);
        it = next;
    }
    m->head = NULL;
    m->current = NULL;
    m->press_from = NULL;
    m->pressed = 0;
}

void cmvs_menus_free(cmvs_menus *m)
{
    int i;
    if (!m) return;
    for (i = 0; i < CMVS_MENUS; i++) items_free(&m->menu[i]);
    free(m);
}

static cmvs_menu *reach(cmvs_menus *m, int menu)
{
    if (!m || menu < 0 || menu >= CMVS_MENUS) return NULL;
    return m->menu[menu].used ? &m->menu[menu] : NULL;
}

static cmvs_menu_item *find(cmvs_menu *m, int id)
{
    cmvs_menu_item *it;
    for (it = m->head; it; it = it->next)
        if (it->id == id) return it;
    return NULL;
}

static cmvs_menu_item *last(cmvs_menu *m)
{
    cmvs_menu_item *it = m->head;
    if (!it) return NULL;
    while (it->next) it = it->next;
    return it;
}

int cmvs_menu_bind(cmvs_menus *m, int menu, int object)
{
    if (!m || menu < 0 || menu >= CMVS_MENUS) return 0;
    if (object < 0 || object >= CMVS_OBJECTS) return 0;
    /* A second bind replaces the menu outright, as 0x004690C0 does: it frees
     * whatever was in the slot before it allocates. */
    items_free(&m->menu[menu]);
    m->menu[menu].used = 1;
    m->menu[menu].object = object;
    return 1;
}

void cmvs_menu_drop(cmvs_menus *m, int menu)
{
    if (!m || menu < 0 || menu >= CMVS_MENUS) return;
    items_free(&m->menu[menu]);
    m->menu[menu].used = 0;
    m->menu[menu].object = 0;
}

int cmvs_menu_add(cmvs_menus *m, int menu, int id)
{
    cmvs_menu *mm = reach(m, menu);
    cmvs_menu_item *it, *tail;
    if (!mm) return 0;
    it = calloc(1, sizeof *it);
    if (!it) return 0;
    it->enabled = 1;
    it->id = id;
    it->prev_id = -1;
    it->next_id = -1;
    tail = last(mm);
    if (!tail) {
        mm->head = it;
    } else {
        /* The list is doubly linked by ID rather than by pointer: an item
         * carries the id above it and the id below it, and -1 at either end is
         * what makes the poll wrap around. */
        tail->next = it;
        it->prev_id = tail->id;
        tail->next_id = it->id;
    }
    return 1;
}

void cmvs_menu_rect(cmvs_menus *m, int menu, int id, int x, int y, int w, int h)
{
    cmvs_menu *mm = reach(m, menu);
    cmvs_menu_item *it = mm ? find(mm, id) : NULL;
    if (!it) return;
    it->x = x;
    it->y = y;
    it->w = w;
    it->h = h;
}

void cmvs_menu_state(cmvs_menus *m, int menu, int id, int state, int part,
                     int sx, int sy, int sw, int sh, int ox, int oy)
{
    cmvs_menu *mm = reach(m, menu);
    cmvs_menu_item *it = mm ? find(mm, id) : NULL;
    cmvs_menu_sprite *s;
    if (!it || state < 0 || state >= CMVS_MENU_STATES) return;
    /* One argument fills one state. 0x00453970 has a second mapping, in which
     * three arguments fill the four blocks, behind the global at 0x0058E364;
     * that flag is a legacy mode and nothing in this game sets it. */
    s = &it->state[state];
    s->part = part;
    s->sx = sx;
    s->sy = sy;
    s->sw = sw;
    s->sh = sh;
    s->ox = ox;
    s->oy = oy;
}

/* Showing a state IS the drawing: the part's source rectangle and offset are
 * rewritten in place, exactly as 0x00433310 and 0x00433330 do. */
static void wear(cmvs_scene *scene, int object, const cmvs_menu_sprite *s)
{
    if (!scene || !s) return;
    cmvs_scene_source(scene, object, s->part, s->sx, s->sy, s->sw, s->sh);
    cmvs_scene_offset(scene, object, s->part, s->ox, s->oy);
}

void cmvs_menu_finish(cmvs_menus *m, int menu, cmvs_scene *scene)
{
    cmvs_menu *mm = reach(m, menu);
    cmvs_menu_item *it;
    if (!mm) return;
    for (it = mm->head; it; it = it->next) {
        const cmvs_menu_sprite *s = &it->state[it->enabled ? 0 : 3];
        wear(scene, mm->object, s);
        cmvs_scene_show(scene, mm->object, s->part, 1);
    }
    mm->current = NULL;
}

int cmvs_menu_current(const cmvs_menus *m, int menu)
{
    const cmvs_menu *mm;
    if (!m || menu < 0 || menu >= CMVS_MENUS) return -1;
    mm = &m->menu[menu];
    return mm->current ? mm->current->id : -1;
}

int cmvs_menu_poll(cmvs_menus *m, int menu, cmvs_input *in, cmvs_scene *scene)
{
    cmvs_menu *mm = reach(m, menu);
    cmvs_menu_item *hit = NULL, *it;
    if (!mm || !in) return CMVS_MENU_NONE;

    /* A direction moves the selection along the id links, wrapping at either
     * end, and then WARPS THE POINTER to the middle of what it selected. The
     * original does it with SetCursorPos so that the hit test below - the only
     * thing that actually decides what is selected - agrees with the pad. */
    if (in->up_pressed) {
        in->up_pressed = 0;
        if (mm->current && mm->current->prev_id >= 0) hit = find(mm, mm->current->prev_id);
        else hit = last(mm);
    }
    if (in->down_pressed) {
        in->down_pressed = 0;
        if (mm->current && mm->current->next_id >= 0) hit = find(mm, mm->current->next_id);
        else hit = mm->head;
    }
    if (hit) cmvs_input_move(in, hit->x + hit->w / 2, hit->y + hit->h / 2);

    /* The pointer decides, and the LAST item whose rectangle holds it wins,
     * which is what the walk at 0x00453F30 does with overlapping rectangles. */
    if (in->have_pointer) {
        for (it = mm->head; it; it = it->next) {
            if (!it->enabled) continue;
            if (in->x < it->x || in->y < it->y) continue;
            if (in->x >= it->x + it->w || in->y >= it->y + it->h) continue;
            hit = it;
        }
    }

    if (hit != mm->current) {
        if (mm->current) wear(scene, mm->object, &mm->current->state[0]);
        if (hit) wear(scene, mm->object, &hit->state[1]);
        mm->pressed = 0;
        mm->current = hit;
    }

    if (in->left_pressed) mm->press_from = mm->current;
    if (in->left_released) {
        in->left_released = 0;
        in->left_pressed = 0;
        /* A click counts only where it began: press START, drag off it and let
         * go and the game is told so rather than started. */
        if (mm->current && mm->current == mm->press_from) return mm->current->id;
        return CMVS_MENU_ELSEWHERE;
    }
    if (!in->left_held) {
        mm->press_from = NULL;
        if (mm->pressed) {
            if (mm->current) wear(scene, mm->object, &mm->current->state[1]);
            mm->pressed = 0;
        }
    } else if (!mm->pressed && mm->current) {
        wear(scene, mm->object, &mm->current->state[2]);
        mm->pressed = 1;
    }

    if (in->right_released) {
        in->right_released = 0;
        in->right_pressed = 0;
        return CMVS_MENU_CANCEL;
    }
    return CMVS_MENU_NONE;
}
