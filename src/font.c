/* popen, for fontconfig on the desktop; the console never reaches it. */
#define _POSIX_C_SOURCE 200809L

#include "font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cp932.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
/* The header offers a great deal more than glyph rasterising; the parts this
   engine never calls are not a defect in it. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_truetype.h"
#pragma GCC diagnostic pop

#define MAX_FONT (64u * 1024 * 1024)

struct cmvs_font {
    char path[1024];
    uint8_t *file;
    size_t size;
    stbtt_fontinfo info;
    uint8_t *glyph;          /* the last rasterised bitmap, freed on the next */
};

/*
 * The console's own Japanese faces, newest naming first. Android has carried a
 * CJK face since Droid Sans Fallback and carries Noto Sans CJK today; the
 * Retroid is an Android 12 device, so the .ttc is what it will find. These are
 * paths on the device, not files in this repository.
 */
static const char *SYSTEM_FONTS[] = {
    "/system/fonts/NotoSansCJK-Regular.ttc",
    "/system/fonts/NotoSansJP-Regular.otf",
    "/system/fonts/NotoSansCJKjp-Regular.otf",
    "/system/fonts/DroidSansJapanese.ttf",
    "/system/fonts/DroidSansFallback.ttf",
    "/system/fonts/DroidSansFallbackFull.ttf",
};
static const int SYSTEM_FONT_COUNT = (int) (sizeof SYSTEM_FONTS / sizeof SYSTEM_FONTS[0]);

static int readable(const char *path)
{
    FILE *f;
    if (!path || !*path) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/*
 * Whether a face can actually draw this game's text.
 *
 * The original asks Windows for "MS Gothic" and gets one face that covers both
 * the kana and the Latin the scripts mix into the same line. A CJK *fallback*
 * face covers the kana and has no Latin ink at all, so a machine that answers
 * with one would draw ChronoClock's first line, "July... Summertime.", as
 * nineteen blanks. Checking both alphabets is what the original gets for free.
 */
static int covers_the_game(const char *path)
{
    cmvs_font *f = cmvs_font_open(path);
    int w = 0, h = 0, ok;
    if (!f) return 0;
    ok = cmvs_font_glyph(f, 'A', 30, 0, &w, &h, NULL, NULL) != NULL && w > 0 && h > 0;
    if (ok) ok = cmvs_font_glyph(f, 0x3042, 30, 0, &w, &h, NULL, NULL) != NULL && w > 0 && h > 0;
    cmvs_font_free(f);
    return ok;
}

/*
 * fontconfig's answers, asked for through fc-match rather than linked against.
 * The desktop runner is a development harness; making the engine depend on
 * libfontconfig for it would put a library in the Android build that Android
 * does not have. The console never reaches this: it finds its face above.
 *
 * --sort rather than a single match, because the best-named answer is not
 * always one that can draw the game (see covers_the_game).
 */
static int ask_fontconfig(const char *pattern, char *path, size_t pathlen)
{
    char command[512];
    char line[1024];
    FILE *pipe;
    int found = 0, tried = 0;

    snprintf(command, sizeof command,
             "fc-match --sort -f '%%{file}\\n' '%s' 2>/dev/null", pattern);
    pipe = popen(command, "r");
    if (!pipe) return 0;
    while (!found && tried < 24 && fgets(line, sizeof line, pipe)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == 0x0A || line[n - 1] == 0x0D)) line[--n] = 0;
        if (n == 0) continue;
        tried++;
        if (!covers_the_game(line)) continue;
        snprintf(path, pathlen, "%s", line);
        found = 1;
    }
    pclose(pipe);
    return found;
}

int cmvs_font_find(const char *override, const char *wanted, char *path, size_t pathlen)
{
    int i;

    if (!path || pathlen < 2) return 0;
    path[0] = 0;

    if (override && *override) {
        if (!readable(override)) return 0;
        snprintf(path, pathlen, "%s", override);
        return 1;
    }
    for (i = 0; i < SYSTEM_FONT_COUNT; i++) {
        if (readable(SYSTEM_FONTS[i]) && covers_the_game(SYSTEM_FONTS[i])) {
            snprintf(path, pathlen, "%s", SYSTEM_FONTS[i]);
            return 1;
        }
    }
    if (wanted && *wanted) {
        char pattern[256];
        /* :lang=ja is the part that matters. Without it fontconfig answers a
         * Latin face for a family it does not have, and every kanji then
         * rasterises as a missing glyph - which looks like a broken engine
         * rather than a machine with no Japanese font on it. */
        snprintf(pattern, sizeof pattern, "%s:lang=ja", wanted);
        if (ask_fontconfig(pattern, path, pathlen)) return 1;
    }
    if (ask_fontconfig("sans-serif:lang=ja", path, pathlen)) return 1;
    path[0] = 0;
    return 0;
}

cmvs_font *cmvs_font_open(const char *path)
{
    cmvs_font *f;
    FILE *file;
    long size;
    int offset;

    if (!path || !*path) return NULL;
    file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    size = ftell(file);
    if (size <= 0 || (unsigned long) size > MAX_FONT) { fclose(file); return NULL; }
    rewind(file);

    f = calloc(1, sizeof *f);
    if (!f) { fclose(file); return NULL; }
    f->file = malloc((size_t) size);
    if (!f->file || fread(f->file, 1, (size_t) size, file) != (size_t) size) {
        fclose(file);
        free(f->file);
        free(f);
        return NULL;
    }
    fclose(file);
    f->size = (size_t) size;
    snprintf(f->path, sizeof f->path, "%s", path);

    /* A .ttc holds several faces; face 0 is the regular one in every CJK
     * collection Android ships. */
    offset = stbtt_GetFontOffsetForIndex(f->file, 0);
    if (offset < 0 || !stbtt_InitFont(&f->info, f->file, offset)) {
        free(f->file);
        free(f);
        return NULL;
    }
    return f;
}

void cmvs_font_free(cmvs_font *f)
{
    if (!f) return;
    if (f->glyph) stbtt_FreeBitmap(f->glyph, NULL);
    free(f->file);
    free(f);
}

const char *cmvs_font_path(const cmvs_font *f) { return f ? f->path : ""; }

/*
 * The engine asks for a size in pixels and means the em square, the way
 * CreateFontA's height does: a size of 30 lays out on a 30-pixel grid, which
 * is what makes the game's own 30-pixel cells line up.
 */
static float scale_for(cmvs_font *f, int size)
{
    return stbtt_ScaleForMappingEmToPixels(&f->info, (float) size);
}

const uint8_t *cmvs_font_glyph(cmvs_font *f, unsigned code, int size, int cell,
                               int *w, int *h, int *left, int *top)
{
    float scale, wide;
    int glyph, x0, y0, x1, y1, ascent, descent, gap, advance = 0, bearing = 0;
    uint8_t *bitmap;

    if (w) *w = 0;
    if (h) *h = 0;
    if (left) *left = 0;
    if (top) *top = 0;
    if (!f || size <= 0 || size > 512) return NULL;

    glyph = stbtt_FindGlyphIndex(&f->info, (int) code);
    if (glyph == 0) return NULL;

    scale = scale_for(f, size);
    /* The cell the layout allotted, honoured by condensing rather than by
     * overflowing into the next character. See the header. */
    wide = scale;
    stbtt_GetGlyphHMetrics(&f->info, glyph, &advance, &bearing);
    if (cell > 0 && advance > 0 && advance * scale > (float) cell)
        wide = (float) cell / (float) advance;

    stbtt_GetGlyphBitmapBox(&f->info, glyph, wide, scale, &x0, &y0, &x1, &y1);
    if (x1 <= x0 || y1 <= y0) return NULL;   /* a space: an advance, no ink */

    bitmap = stbtt_GetGlyphBitmap(&f->info, wide, scale, glyph, w, h, NULL, NULL);
    if (!bitmap) return NULL;
    if (f->glyph) stbtt_FreeBitmap(f->glyph, NULL);
    f->glyph = bitmap;

    stbtt_GetFontVMetrics(&f->info, &ascent, &descent, &gap);
    (void) gap;
    if (left) *left = x0;
    /* y0 is measured up from the baseline; the caller works from the top of
     * the line box, which is where the em square starts. */
    if (top) *top = (int) (ascent * scale + 0.5f) + y0;
    return bitmap;
}

int cmvs_font_advance(cmvs_font *f, unsigned code, int size)
{
    int advance = 0, bearing = 0;
    if (!f || size <= 0) return 0;
    stbtt_GetCodepointHMetrics(&f->info, (int) code, &advance, &bearing);
    return (int) (advance * scale_for(f, size) + 0.5f);
}

unsigned cmvs_cp932_next(const char *text, size_t *at)
{
    unsigned char lead, trail;
    int row;

    lead = (unsigned char) text[*at];
    if (lead == 0) return 0;
    if (lead < 0x80 || (lead >= 0xA1 && lead <= 0xDF)) {
        (*at)++;
        /* Half-width katakana sit at 0xA1..0xDF in cp932 and at U+FF61 up in
         * Unicode; everything below 0x80 is ASCII, which cp932 keeps. */
        return lead >= 0xA1 ? (unsigned) (0xFF61 + (lead - 0xA1)) : lead;
    }
    trail = (unsigned char) text[*at + 1];
    if (trail < CMVS_CP932_TRAIL_FIRST || trail > CMVS_CP932_TRAIL_LAST) {
        (*at)++;
        return 0xFFFD;
    }
    for (row = 0; row < CMVS_CP932_ROWS; row++) {
        if (cmvs_cp932_lead[row] != lead) continue;
        {
            unsigned code = cmvs_cp932_table[row][trail - CMVS_CP932_TRAIL_FIRST];
            *at += 2;
            return code ? code : 0xFFFD;
        }
    }
    (*at)++;
    return 0xFFFD;
}
