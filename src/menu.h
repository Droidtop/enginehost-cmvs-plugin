/*
 * The menus: the third object family, and the one that makes a title screen a
 * screen you can press rather than a picture.
 *
 * There are six of them, in the table at +0xc6c, and each is a linked list of
 * 0x6c-byte items (allocated in 0x00453870). An item is not drawn by the menu:
 * it names a PART of one graphic object - the object command 0x210 bound to the
 * menu - and four SPRITE STATES, each of which is a source rectangle in that
 * object's bitmap plus an offset. Pointing at an item swaps its part to the
 * hover rectangle, holding the button swaps it to the pressed one; that is the
 * whole of the visual feedback, and it is why one 1280x1024 sheet holds every
 * caption three times over.
 *
 * Item layout, from the setters (offsets are from the node, which is what the
 * list links; the engine's own pointers are to the node plus four):
 *   +0x00 next   +0x04 enabled   +0x08 id
 *   +0x0c +0x10 +0x14 +0x18   the hit rectangle x, y, w, h (0x00453930)
 *   +0x1c +0x20               the ids above and below it, -1 at the ends
 *   +0x2c +0x3c +0x4c +0x5c   four sprite states, 0x10 bytes apiece
 *                             { int part; short sx, sy, sw, sh, ox, oy }
 * State 0 is the normal look, 1 the hover, 2 the pressed and 3 the DISABLED
 * one, which is the block 0x00453B40 uses when the item's enabled flag is 0.
 */
#ifndef CMVS_MENU_H
#define CMVS_MENU_H

#include "input.h"
#include "scene.h"

#define CMVS_MENUS        6    /* the bound every accessor checks */
#define CMVS_MENU_STATES  4

typedef struct cmvs_menus cmvs_menus;

/* What a poll reports, which is what the script branches on. Anything else is
 * an item id. */
#define CMVS_MENU_NONE      (-1)   /* nothing happened this frame */
#define CMVS_MENU_CANCEL    (-2)   /* the right button, 0x00454120 */
#define CMVS_MENU_ELSEWHERE (-10)  /* a click that began on another item */

cmvs_menus *cmvs_menus_new(void);
void cmvs_menus_free(cmvs_menus *m);

int  cmvs_menu_bind(cmvs_menus *m, int menu, int object);   /* 0x210, 0x004690C0 */
void cmvs_menu_drop(cmvs_menus *m, int menu);               /* 0x211, 0x00469370 */
int  cmvs_menu_add(cmvs_menus *m, int menu, int id);        /* 0x212, 0x00453870 */
void cmvs_menu_rect(cmvs_menus *m, int menu, int id,
                    int x, int y, int w, int h);            /* 0x213, 0x00453930 */
void cmvs_menu_state(cmvs_menus *m, int menu, int id, int state, int part,
                     int sx, int sy, int sw, int sh,
                     int ox, int oy);                       /* 0x214, 0x00453970 */
void cmvs_menu_finish(cmvs_menus *m, int menu, cmvs_scene *scene);  /* 0x215, 0x00453B40 */
int  cmvs_menu_current(const cmvs_menus *m, int menu);      /* 0x216, 0x00453BF0 */
int  cmvs_menu_poll(cmvs_menus *m, int menu, cmvs_input *in,
                    cmvs_scene *scene);                     /* 0x217, 0x00453DC0 */

#endif
