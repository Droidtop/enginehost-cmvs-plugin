#include "text.h"

#include <stdlib.h>
#include <string.h>

/* cp932's lead bytes, the test 0x00406c20 makes on every character. */
static int lead_byte(unsigned char c)
{
    return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC);
}

void cmvs_text_init(cmvs_text *t)
{
    /* 0x00451ee0, which is where the engine puts a layer's text object back
     * to its defaults, and every number here is one it writes. */
    memset(t, 0, sizeof *t);
    t->size = 0x1B;
    t->colour = 0xFFFFFF;
    t->colour2 = 0xFFFFFF;
    t->edge = 0;
    t->kinsoku = 1;
    t->speed = 0x64;
    t->mode = 0;
    t->fade = 0;
    t->visible = 1;
}

void cmvs_text_free(cmvs_text *t)
{
    free(t->glyph);
    t->glyph = NULL;
    t->glyphs = t->capacity = 0;
}

void cmvs_text_clear(cmvs_text *t)
{
    t->glyphs = 0;
    t->pen_x = t->rx;
    t->pen_y = t->ry;
    /* The line is gone and so is its reveal: the next one starts from zero. */
    t->clock = 0;
    t->next_start = 0;
}

/*
 * The per-character step, straight out of 0x004511f6: the speed at +0x8c
 * doubled and divided by ten, which for the default 0x64 is 20 ms. A wait
 * descriptor scales it by its own first field instead of by two; no command
 * ChronoClock calls passes one, so the doubling is what runs here.
 */
static int step_ms(const cmvs_text *t)
{
    return t->speed * 2 / 10;
}

void cmvs_text_tick(cmvs_text *t, int ms)
{
    if (t && ms > 0) t->clock += ms;
}

int cmvs_text_revealing(const cmvs_text *t)
{
    return t && t->glyphs > 0 && t->glyph[t->glyphs - 1].start > t->clock;
}

void cmvs_text_reveal_all(cmvs_text *t)
{
    if (t && t->glyphs > 0 && t->glyph[t->glyphs - 1].start > t->clock)
        t->clock = t->glyph[t->glyphs - 1].start;
}

void cmvs_text_box(cmvs_text *t, int x, int y, int w, int h)
{
    t->rx = x; t->ry = y; t->rw = w; t->rh = h;
    t->pen_x = x; t->pen_y = y;
}

void cmvs_text_size(cmvs_text *t, int size, int gap, int lead)
{
    /* 0x00450450 takes -1 for "leave this one alone", which is how command
     * 0x143 sets the size on its own. */
    if (size > 0) t->size = size;
    if (gap >= 0) t->gap = gap;
    if (lead >= 0) t->lead = lead;
}

void cmvs_text_colours(cmvs_text *t, uint32_t a, uint32_t b)
{
    /* 0x00450520 refuses a colour with anything in its top byte, which is how
     * a script says "keep the one you have". */
    if (!(a & 0xFF000000u)) t->colour = a;
    if (!(b & 0xFF000000u)) t->colour2 = b;
}

void cmvs_text_edge(cmvs_text *t, uint32_t edge) { t->edge = edge; }
void cmvs_text_pen(cmvs_text *t, int x, int y) { t->pen_x = x; t->pen_y = y; }
void cmvs_text_show(cmvs_text *t, int visible) { t->visible = visible ? 1 : 0; }

static int append(cmvs_text *t, unsigned code, int x, int y)
{
    cmvs_glyph *g;
    if (t->glyphs == t->capacity) {
        int want = t->capacity ? t->capacity * 2 : 64;
        cmvs_glyph *bigger = realloc(t->glyph, (size_t) want * sizeof *bigger);
        if (!bigger) return 0;
        t->glyph = bigger;
        t->capacity = want;
    }
    g = &t->glyph[t->glyphs++];
    g->code = code;
    g->x = x;
    g->y = y;
    g->size = t->size;
    g->colour = t->colour;
    g->edge = t->edge;
    g->start = t->next_start;
    t->next_start += step_ms(t);
    return 1;
}

/* The four characters a line may start with even when it is full: 0x00450a2b
 * lists them and they are 、 。 」 ) in cp932. */
static int hangs(unsigned short code)
{
    return code == 0x8141 || code == 0x8142 || code == 0x8176 || code == 0x8178;
}

static void newline(cmvs_text *t)
{
    t->pen_x = t->rx;
    t->pen_y += t->size + t->lead;
}

void cmvs_text_draw(cmvs_text *t, const char *s)
{
    int limit;
    size_t p = 0;

    if (!s) return;
    limit = t->rx + t->rw - t->size - t->gap;

    while (s[p]) {
        unsigned char c = (unsigned char) s[p];

        if (c == 0x5C) {                      /* 0x004515e3, the escapes */
            char n = s[p + 1];
            if (n == 'c') { cmvs_text_clear(t); p += 2; continue; }
            if (n == 'n') { newline(t); p += 2; continue; }
            if (n == 'w') { p += 3; continue; }   /* \wc and \ws: wait marks */
            p += 2;                                /* \t, and the table's rest */
            continue;
        }
        if (c == 0x7B) {                      /* {base/reading}: ruby */
            /* The base characters are laid out like any others; the reading
             * that follows the slash needs the half-size line above the box
             * (+0xb0) that no command has set yet, so it is not drawn rather
             * than drawn in the wrong place. */
            size_t q = p + 1;
            while (s[q] && s[q] != 0x2F && s[q] != 0x7D) {
                unsigned code = cmvs_cp932_next(s, &q);
                if (t->pen_x >= limit) newline(t);
                append(t, code, t->pen_x, t->pen_y);
                t->pen_x += t->size + t->gap;
            }
            while (s[q] && s[q] != 0x7D) q++;
            p = s[q] ? q + 1 : q;
            continue;
        }
        if (lead_byte(c)) {
            unsigned short raw = (unsigned short) ((c << 8) | (unsigned char) s[p + 1]);
            unsigned code = cmvs_cp932_next(s, &p);
            if (t->pen_x >= limit && !(t->kinsoku && hangs(raw))) newline(t);
            append(t, code, t->pen_x, t->pen_y);
            t->pen_x += t->size + t->gap;
            continue;
        }
        if (t->pen_x >= limit) newline(t);
        append(t, c, t->pen_x, t->pen_y);
        p++;
        /* A single-byte character is half a cell wide: 0x00450a96. */
        t->pen_x += t->size / 2 + t->gap;
    }
}

int cmvs_text_measure(const cmvs_text *t, const char *s, int *widest, int *last)
{
    int limit = t->rx + t->rw - t->size - t->gap;
    int pen = t->pen_x, broke = 0, on_line = 0, most = 0;
    size_t p = 0;

    if (widest) *widest = 0;
    if (last) *last = 0;
    if (!s) return 1;

    while (s[p]) {
        unsigned char c = (unsigned char) s[p];
        if (c == 0x5C) {
            char n = s[p + 1];
            if (n == 'n') { broke++; pen = t->rx; on_line = 0; p += 2; continue; }
            p += (n == 'w') ? 3 : 2;
            continue;
        }
        if (c == 0x7B) { p++; continue; }
        if (lead_byte(c)) {
            unsigned short raw = (unsigned short) ((c << 8) | (unsigned char) s[p + 1]);
            if (pen >= limit && !(t->kinsoku && hangs(raw))) {
                broke++; pen = t->rx; on_line = 0;
            }
            pen += t->size + t->gap;
            on_line += 2;
            p += 2;
        } else {
            if (pen >= limit) { broke++; pen = t->rx; on_line = 0; }
            pen += t->size / 2 + t->gap;
            on_line += 1;
            p += 1;
        }
        if (on_line > most) most = on_line;
    }
    if (widest) *widest = most;
    if (last) *last = on_line;
    (void) pen;
    return broke + 1;
}

/* ------------------------------------------------------------------- ink */

static void ink(uint8_t *frame, int width, int height, int x, int y,
                uint32_t colour, int alpha)
{
    uint8_t *px;
    if (alpha <= 0 || x < 0 || y < 0 || x >= width || y >= height) return;
    px = frame + 4 * ((size_t) y * width + x);
    /* A COLORREF is 0x00bbggrr and the frame is BGRA. */
    if (alpha >= 255) {
        px[0] = (uint8_t) ((colour >> 16) & 0xFF);
        px[1] = (uint8_t) ((colour >> 8) & 0xFF);
        px[2] = (uint8_t) (colour & 0xFF);
        px[3] = 0xFF;
        return;
    }
    px[0] = (uint8_t) ((((colour >> 16) & 0xFF) * alpha + px[0] * (255 - alpha)) / 255);
    px[1] = (uint8_t) ((((colour >> 8) & 0xFF) * alpha + px[1] * (255 - alpha)) / 255);
    px[2] = (uint8_t) (((colour & 0xFF) * alpha + px[2] * (255 - alpha)) / 255);
    px[3] = 0xFF;
}

void cmvs_text_compose(const cmvs_text *t, cmvs_font *font, uint8_t *frame,
                       int width, int height, int ox, int oy)
{
    int i;
    if (!t || !font || !frame || !t->visible) return;
    for (i = 0; i < t->glyphs; i++) {
        const cmvs_glyph *g = &t->glyph[i];
        int gw = 0, gh = 0, left = 0, top = 0, row, col;
        if (g->start > t->clock) break;   /* not typed yet */
        const uint8_t *bits = cmvs_font_glyph(font, g->code, g->size,
                                              &gw, &gh, &left, &top);
        if (!bits) continue;
        for (row = 0; row < gh; row++)
            for (col = 0; col < gw; col++)
                ink(frame, width, height,
                    ox + g->x + left + col, oy + g->y + top + row,
                    g->colour, bits[row * gw + col]);
    }
}
