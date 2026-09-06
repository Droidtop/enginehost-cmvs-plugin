/*
 * The text a layer draws: the message window's lines.
 *
 * Every text command (0x141 to 0x15d) reaches its layer through
 * [machine + layer*4 + 0xb90] and then that layer object's +0x9dc, so there is
 * one text object per display layer and it is addressed exactly the way the
 * sprite commands address the layer's parts. The field offsets in the comments
 * are that object's, read off the setters in cmvs32.exe:
 *
 *   +0x0c +0x10 +0x14 +0x18   the box: x, y, w, h        (0x00450420, cmd 0x145)
 *   +0x1c +0x20 +0x24         size, char gap, line gap   (0x00450450, cmd 0x143)
 *   +0x6c                     kinsoku on                 (cmd 0x148's neighbour)
 *   +0x70 +0x74               two colours                (0x00450520, cmd 0x146)
 *   +0x7c                     the edge colour            (0x004505b0, cmd 0x155)
 *   +0x90                     shown                      (0x004505e0)
 *   +0x98 +0x9c               the pen                    (0x00450550, cmd 0x14f)
 *   +0xa0                     the glyphs drawn so far    (0x004501b0 links them)
 *
 * A colour here is a Windows COLORREF, because the original hands it to
 * SetBkColor: the low byte is red.
 */
#ifndef CMVS_TEXT_H
#define CMVS_TEXT_H

#include <stddef.h>
#include <stdint.h>

#include "font.h"

/* A laid-out character: where it goes and what it is. The engine keeps these
 * as graphic objects on a list at +0xa0; they are the same thing. */
typedef struct {
    unsigned code;          /* Unicode, decoded from the script's cp932 */
    int x, y;               /* the pen, in the layer's coordinates */
    int size;               /* the em square it was laid out on */
    uint32_t colour;
    uint32_t edge;
} cmvs_glyph;

typedef struct {
    int rx, ry, rw, rh;     /* the box */
    int size, gap, lead;    /* +0x1c, +0x20, +0x24 */
    int kinsoku;            /* +0x6c: do not start a line with 、。」) */
    uint32_t colour, colour2, edge;
    int visible;
    int pen_x, pen_y;

    cmvs_glyph *glyph;
    int glyphs, capacity;
} cmvs_text;

void cmvs_text_init(cmvs_text *t);
void cmvs_text_free(cmvs_text *t);

/* Command 0x14a / 0x141 (0x004506a0): the line is gone and the pen goes back
 * to the top left of the box. */
void cmvs_text_clear(cmvs_text *t);

void cmvs_text_box(cmvs_text *t, int x, int y, int w, int h);
void cmvs_text_size(cmvs_text *t, int size, int gap, int lead);
void cmvs_text_colours(cmvs_text *t, uint32_t a, uint32_t b);
void cmvs_text_edge(cmvs_text *t, uint32_t edge);
void cmvs_text_pen(cmvs_text *t, int x, int y);
void cmvs_text_show(cmvs_text *t, int visible);

/*
 * Command 0x152: lay a cp932 string out from the pen, appending its characters
 * to the line. The layout is 0x00451120's: a double-byte character advances by
 * size + gap and a single-byte one by half that, a line wraps once the pen
 * passes box.x + box.w - size - gap, and a new line drops by size + lead.
 * `\n` breaks the line, `\c` clears it, `\t`, `\wc` and `\ws` are the wait
 * marks the reveal reads and cost no space.
 */
void cmvs_text_draw(cmvs_text *t, const char *cp932);

/*
 * Command 0x112 (0x004509a0): how the string WOULD lay out, without drawing
 * it. The RETURN is how many lines it takes; `widest` is the most characters
 * any one of them holds and `last` how many are on the final one. A character
 * counts as one byte here, so a double-byte one counts two, which is what the
 * function's own counter does.
 */
int cmvs_text_measure(const cmvs_text *t, const char *cp932,
                      int *widest, int *last);

/* Rasterises the line into a BGRA frame at (ox, oy) - the layer's own position,
 * because a layer's text sits inside the window the layer draws. */
void cmvs_text_compose(const cmvs_text *t, cmvs_font *font, uint8_t *frame,
                       int width, int height, int ox, int oy);

#endif
