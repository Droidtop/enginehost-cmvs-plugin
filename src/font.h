/*
 * Glyphs, drawn with a font the engine does not own.
 *
 * ChronoClock ships no font file. cmvs.cfg names Windows families instead
 * (FONT=MS Gothic, FONT=Meiryo, both in cp932) and cmvs32.exe rasterises each
 * character itself through GetGlyphOutlineA into an atlas of its own. We do
 * the same thing with the one part Windows was providing: a rasteriser. That
 * is stb_truetype, a single public-domain header in third_party/, and the face
 * comes from the machine the game is running on - fontconfig's answer on the
 * desktop, /system/fonts on the console. No font file is committed to this
 * repository and none is shipped in the plugin bundle.
 */
#ifndef CMVS_FONT_H
#define CMVS_FONT_H

#include <stddef.h>
#include <stdint.h>

typedef struct cmvs_font cmvs_font;

/*
 * Finds a face to draw a cp932 game with and writes its path into `path`.
 *
 * `wanted` is the family cmvs.cfg asked for and may be NULL. The order is the
 * one the caller can predict: an explicit path (`override`) always wins, then
 * the console's own CJK faces under /system/fonts, then fontconfig's match for
 * the wanted family restricted to Japanese, then fontconfig's match for any
 * Japanese face at all. Returns 0 when the machine has no Japanese font, which
 * is a fact worth reporting rather than papering over with a Latin one.
 */
int cmvs_font_find(const char *override, const char *wanted,
                   char *path, size_t pathlen);

/* Opens a TrueType/OpenType file, or the first face of a .ttc. */
cmvs_font *cmvs_font_open(const char *path);
void cmvs_font_free(cmvs_font *f);

const char *cmvs_font_path(const cmvs_font *f);

/*
 * Rasterises one Unicode code point at `size` pixels em.
 *
 * The bitmap is 8-bit coverage, `w` by `h`, and belongs to the font: it is
 * valid until the next call. `left` and `top` are where its top-left corner
 * goes relative to the pen position at the top of the line box, so a caller
 * that knows only the cell it is filling needs no metrics of its own.
 * Returns NULL for a code point the face has no glyph for, and for a space,
 * which has an advance and no pixels.
 */
const uint8_t *cmvs_font_glyph(cmvs_font *f, unsigned code, int size,
                               int *w, int *h, int *left, int *top);

/* How far the pen moves after that code point, at that size. */
int cmvs_font_advance(cmvs_font *f, unsigned code, int size);

/*
 * One code point out of a cp932 string, and how many bytes it took. A byte
 * that is not valid cp932 comes back as U+FFFD and costs one byte, so a bad
 * byte in a script cannot lose the rest of the line.
 */
unsigned cmvs_cp932_next(const char *text, size_t *at);

#endif
