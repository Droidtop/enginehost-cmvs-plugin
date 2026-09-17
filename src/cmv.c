#include "cmv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jbp.h"

#define CMV_HEADER 0x2C     /* the header 0x00432240 reads before the table */
#define CMV_ENTRY  20       /* frame_no, size, expanded, codec, offset */
#define CMV_TILE   256      /* the square the player locks its surface in */

struct cmv_movie {
    const uint8_t *data;
    int size;

    int base;               /* +0x04, what a table entry's offset is relative to */
    int frames;             /* +0x10 */
    int fps;                /* +0x14 */
    int step;               /* +0x18 */
    int width, height;      /* +0x1c, +0x20 */
    int has_audio;          /* +0x28 */

    int generation;         /* the fourth magic byte, '5' .. '9' */
    int table;              /* CMV_HEADER */
    int block;              /* the movie block, or -1 */
    int mb_w, mb_h;         /* the block's +4, or the picture's own grid */
    int mask, mask_len;     /* block +8; mask is -1 when there is no block */
    int intra;              /* the intra chunk inside the block, or -1 */

    int tiles_x, tiles_y;

    int aligned_w, aligned_h;
    int stride;
    uint8_t *pixels;

    int16_t *dc;            /* one run of DC coefficients, reused per chunk */
    int dc_room;
};

static void fail(char *err, size_t errlen, const char *msg)
{
    if (err && errlen) { snprintf(err, errlen, "%s", msg); }
}

static int32_t i32(const uint8_t *b, int o)
{
    return (int32_t) ((uint32_t) b[o] | ((uint32_t) b[o + 1] << 8)
                    | ((uint32_t) b[o + 2] << 16) | ((uint32_t) b[o + 3] << 24));
}

static int u16v(const uint8_t *b, int o) { return b[o] | (b[o + 1] << 8); }

/*
 * A chunk's own macroblock grid. 0x0050c580 reads the two u16 at +0x10 and the
 * alignment out of bits 28..29 of the format word, and every decoder then takes
 * the aligned size divided by sixteen - 0x00513ae3 and 0x00513af8 do it
 * literally. A CMV9 also writes the same grid into its movie block, and the two
 * must agree or the mask is indexed one way and the coefficients another.
 */
static int chunk_grid(const uint8_t *d, int chunk, int *gw, int *gh)
{
    int format = i32(d, chunk + 8);
    int w = u16v(d, chunk + 0x10), h = u16v(d, chunk + 0x12);
    int aw, ah;
    if (w <= 0 || h <= 0) return 0;
    switch (((unsigned) format >> 28) & 3) {
    case 0: aw = (w + 7) & ~7;       ah = (h + 7) & ~7;       break;
    case 1: aw = (w + 0xF) & ~0xF;   ah = (h + 0xF) & ~0xF;   break;
    case 2: aw = (w + 0x1F) & ~0x1F; ah = (h + 0xF) & ~0xF;   break;
    default: return 0;
    }
    *gw = aw >> 4;
    *gh = ah >> 4;
    return *gw > 0 && *gh > 0;
}

/* A frame-table entry's fields. The table has frames + 1 entries and the last
 * one is the audio, not a frame. */
static int entry_at(const cmv_movie *m, int i) { return m->table + CMV_ENTRY * i; }
static int entry_size(const cmv_movie *m, int i) { return i32(m->data, entry_at(m, i) + 4); }
static int entry_codec(const cmv_movie *m, int i) { return i32(m->data, entry_at(m, i) + 12); }
static int entry_offset(const cmv_movie *m, int i) { return i32(m->data, entry_at(m, i) + 16); }

cmv_movie *cmv_open(const uint8_t *data, int size, char *err, size_t errlen)
{
    cmv_movie *m;
    long table_end;
    int generation;

    if (!data || size < CMV_HEADER) { fail(err, errlen, "not a CMV: too short"); return NULL; }
    if (memcmp(data, "CMV", 3)) { fail(err, errlen, "not a CMV: bad magic"); return NULL; }
    /*
     * 0x00431943: the fourth byte is the generation, '5' through '9', and
     * 0x00431509 is the jump table that gives each one its own decoder.
     *
     * CMV6 and CMV9 are decoded here, because their decoders (0x00513a90 and
     * 0x00519250) differ only in where the mask and the DC count come from.
     *
     * CMV7 and CMV8 are not, and it is not a detail: their frames carry an
     * ALPHA PLANE - a CMV8's own header says 32 bits per pixel and its frame
     * table's expanded size is width * height * 4 + 54, a 32-bit BMP - and
     * their decoders (0x00515e40 and 0x00517480) run two helpers, 0x0050ff00
     * and 0x005119c0, that nothing else in the executable calls. That plane has
     * not been read, so they are refused.
     *
     * CMV5 is refused too, for the plainer reason that no game on this machine
     * has one to check against; its decoder (0x00513070) has the same shape as
     * the CMV6 one, which is a reason to expect it to work and not a reason to
     * say it does.
     */
    generation = data[3];
    if (generation != '6' && generation != '9') {
        if (generation >= '5' && generation <= '9')
            fail(err, errlen, generation == '5'
                 ? "CMV5 is not implemented: no game here has one to check against"
                 : "CMV7 and CMV8 carry an alpha plane this decoder has not read");
        else
            fail(err, errlen, "not a CMV: unknown generation");
        return NULL;
    }

    m = calloc(1, sizeof *m);
    if (!m) { fail(err, errlen, "out of memory opening a CMV"); return NULL; }
    m->data = data;
    m->size = size;
    m->base = i32(data, 4);
    m->frames = i32(data, 0x10);
    m->fps = i32(data, 0x14);
    m->step = i32(data, 0x18);
    m->width = i32(data, 0x1C);
    m->height = i32(data, 0x20);
    m->has_audio = i32(data, 0x28);
    m->generation = generation;
    m->table = CMV_HEADER;
    m->block = -1;
    m->mask = -1;
    m->intra = -1;

    if (m->frames <= 0 || m->frames > 1000000
        || m->width <= 0 || m->height <= 0 || m->width > 8192 || m->height > 8192) {
        fail(err, errlen, "CMV header is not sane");
        cmv_close(m);
        return NULL;
    }
    if (m->fps <= 0) m->fps = 24;
    if (m->step <= 0) m->step = 1;

    table_end = (long) m->table + (long) CMV_ENTRY * ((long) m->frames + 1);
    if (table_end > size || m->base < table_end || m->base > size
        || i32(data, 8) > size) {
        fail(err, errlen, "CMV frame table or frame data lies outside the file");
        cmv_close(m);
        return NULL;
    }

    /*
     * 0x004324ab: only a CMV9 has a movie block, and it is everything between
     * the end of the table and the frame data. Its first dword is its own
     * length, the two u16 after it the macroblock grid, and the mask follows at
     * +8 (which is the +8 0x00519250 and 0x00518d20 both add to it).
     */
    if (generation == '9') {
        int block = (int) table_end, len, need;
        if (block + 8 > m->base) {
            fail(err, errlen, "CMV9 movie block is missing");
            cmv_close(m);
            return NULL;
        }
        len = i32(data, block);
        m->mb_w = u16v(data, block + 4);
        m->mb_h = u16v(data, block + 6);
        if (len != m->base - block || m->mb_w <= 0 || m->mb_h <= 0
            || m->mb_w > 1024 || m->mb_h > 1024) {
            fail(err, errlen, "CMV9 movie block is not sane");
            cmv_close(m);
            return NULL;
        }
        m->mask = block + 8;
        m->mask_len = (m->mb_w * m->mb_h + 7) / 8;
        need = 8 + m->mask_len;
        if (len <= need) {
            fail(err, errlen, "CMV9 movie block carries no intra picture");
            cmv_close(m);
            return NULL;
        }
        m->block = block;
        m->intra = m->mask + m->mask_len;
    } else {
        /*
         * A CMV5..CMV8 has no movie block: 0x004324ab builds one for '9' alone,
         * and its own decoder (0x00513a90 for a CMV6) takes the grid from the
         * picture instead, decodes EVERY macroblock of every frame - its DC
         * count is mb_w * mb_h * 6, computed at 0x00513cb9 rather than read out
         * of the chunk, whose +0x24 is zero - and carries its per-frame mask
         * inline, without the u16 length a CMV9 puts in front of it. There is
         * no intra to lay down, which is why 0x00431060 runs for '9' alone.
         */
        int first = m->base + entry_offset(m, 0);
        if (first < 0 || (long) first + 0x2C > size
            || !chunk_grid(data, first, &m->mb_w, &m->mb_h)) {
            fail(err, errlen, "a CMV5..CMV8 movie's first frame is not readable");
            cmv_close(m);
            return NULL;
        }
    }

    m->tiles_x = (m->width + CMV_TILE - 1) / CMV_TILE;
    m->tiles_y = (m->height + CMV_TILE - 1) / CMV_TILE;

    m->aligned_w = m->mb_w * 16;
    m->aligned_h = m->mb_h * 16;
    if (m->aligned_w < m->width || m->aligned_h < m->height) {
        fail(err, errlen, "CMV macroblock grid does not cover the picture");
        cmv_close(m);
        return NULL;
    }
    m->stride = 4 * m->aligned_w;
    m->pixels = calloc(1, (size_t) m->stride * m->aligned_h);
    if (!m->pixels) { fail(err, errlen, "out of memory for a CMV surface"); cmv_close(m); return NULL; }

    return m;
}

void cmv_close(cmv_movie *m)
{
    if (!m) return;
    free(m->pixels);
    free(m->dc);
    free(m);
}

int cmv_width(const cmv_movie *m) { return m ? m->width : 0; }
int cmv_height(const cmv_movie *m) { return m ? m->height : 0; }
int cmv_frames(const cmv_movie *m) { return m ? m->frames : 0; }
int cmv_fps(const cmv_movie *m) { return m ? m->fps : 0; }
int cmv_step(const cmv_movie *m) { return m ? m->step : 1; }
int cmv_mb_width(const cmv_movie *m) { return m ? m->mb_w : 0; }
int cmv_mb_height(const cmv_movie *m) { return m ? m->mb_h : 0; }

/*
 * How many macroblocks a tile holds, and where in the mask its first bit is.
 * A tile is 256x256 pixels, so sixteen macroblocks each way except at the right
 * and bottom edges where it is what is left. This is the geometry 0x00430da0
 * locks the player's surface in, and the per-chunk tile table must agree with
 * it - if it did not, the mask would be indexed one way and the coefficients
 * another, which is a picture made of the right blocks in the wrong places.
 */
static int tile_rows(const cmv_movie *m, int ty)
{
    int left = m->mb_h - ty * 16;
    return left > 16 ? 16 : left;
}

static int tile_cols(const cmv_movie *m, int tx)
{
    int left = m->mb_w - tx * 16;
    return left > 16 ? 16 : left;
}

static int tile_first_bit(const cmv_movie *m, int tile)
{
    int t, bit = 0;
    for (t = 0; t < tile; t++)
        bit += tile_rows(m, t / m->tiles_x) * tile_cols(m, t % m->tiles_x);
    return bit;
}

int cmv_static(const cmv_movie *m, int x, int y)
{
    int tx, ty, bit;
    if (!m || m->mask < 0) return 0;
    if (x < 0 || y < 0 || x >= m->mb_w || y >= m->mb_h) return 0;
    tx = x / 16;
    ty = y / 16;
    bit = tile_first_bit(m, ty * m->tiles_x + tx)
        + (y % 16) * tile_cols(m, tx) + (x % 16);
    if (bit >= m->mb_w * m->mb_h) return 0;
    return (m->data[m->mask + (bit >> 3)] >> (7 - (bit & 7))) & 1;
}
const uint8_t *cmv_pixels(const cmv_movie *m) { return m ? m->pixels : NULL; }
int cmv_stride(const cmv_movie *m) { return m ? m->stride : 0; }

int cmv_audio(const cmv_movie *m, const uint8_t **data, int *size)
{
    int last, off, len;
    if (data) *data = NULL;
    if (size) *size = 0;
    if (!m || !m->has_audio) return 0;
    /* 0x00432642: entry `frames`, the one past the last frame. */
    last = m->frames;
    if (entry_codec(m, last) != 0) return 0;
    len = entry_size(m, last);
    off = entry_offset(m, last);
    if (len <= 0 || off < 0) return 0;
    if ((long) m->base + off + len > m->size) return 0;
    if (data) *data = m->data + m->base + off;
    if (size) *size = len;
    return 1;
}

/* ------------------------------------------------------------------- chunks */

/*
 * One JBPD chunk, which is the whole of the codec.
 *
 * `is_intra` picks 0x00518d20's reading of it rather than 0x00519250's, and the
 * two differ in exactly three ways: the intra's data offset is not stepped past
 * a "JBPD" magic (0x00518d99 against 0x005192bb), the intra decodes the
 * macroblocks whose mask bit is SET and a frame those whose bit is CLEAR
 * (0x00519073 against 0x005195f5), and only a frame carries the second,
 * per-frame mask.
 */
static int decode_chunk(cmv_movie *m, int chunk, int chunk_end, int is_intra,
                        int allow_delta, char *err, size_t errlen)
{
    const uint8_t *d = m->data;
    int format, data_pos, p, i;
    int tiles, tile_table, tile_count;
    int frame_mask = -1, frame_bit = 0, delta = 0;
    int dc_bits, ac_bits, dc_count, dc_used = 0;
    int mask_bit = 0, t, gw = 0, gh = 0;
    int16_t quant_y[64], quant_c[64];
    uint8_t run_table[JBP_RUN_TABLE];
    int32_t freq[0x20];
    jbp_huffman tree_dc, tree_ac;
    jbp_bits bits_dc, bits_ac;
    int prev = 0;

    if (chunk < 0 || chunk + 0x2C > chunk_end || chunk_end > m->size) {
        fail(err, errlen, "a CMV chunk lies outside the file");
        return 0;
    }
    format = i32(d, chunk + 8);
    data_pos = chunk + i32(d, chunk + 4) + (is_intra ? 0 : 4);
    dc_bits = i32(d, chunk + 0x1C);
    ac_bits = i32(d, chunk + 0x20);
    if (!chunk_grid(d, chunk, &gw, &gh) || gw != m->mb_w || gh != m->mb_h) {
        fail(err, errlen, "a CMV chunk is not the size the movie is");
        return 0;
    }
    /*
     * A CMV9 says how many DC coefficients its masked set has (0x00519473 reads
     * the chunk's +0x24); a CMV5..CMV8 codes every macroblock and computes the
     * count from the grid (0x00513cb9), its +0x24 being zero.
     */
    dc_count = m->mask >= 0 ? i32(d, chunk + 0x24) : m->mb_w * m->mb_h * 6;
    if (data_pos < chunk || dc_bits < 0 || ac_bits < 0 || dc_count <= 0
        || dc_count > m->mb_w * m->mb_h * 6) {
        fail(err, errlen, "a CMV chunk header is not sane");
        return 0;
    }
    p = data_pos + 0x90;                       /* two frequency tables and the run table */
    if (p > chunk_end) { fail(err, errlen, "a CMV chunk's tables lie outside it"); return 0; }

    for (i = 0; i < JBP_RUN_TABLE; i++) run_table[i] = (uint8_t) (d[data_pos + 0x80 + i] + 1);
    memset(freq, 0, sizeof freq);
    for (i = 0; i < 16; i++) freq[i] = i32(d, data_pos + 4 * i);
    jbp_huffman_build(&tree_dc, 0x10, freq);
    memset(freq, 0, sizeof freq);
    for (i = 0; i < 16; i++) freq[i] = i32(d, data_pos + 0x40 + 4 * i);
    jbp_huffman_build(&tree_ac, 0x10, freq);

    /*
     * 0x00519308: the quantisation tables are stored in ZIGZAG order and the
     * coefficients are multiplied by them as they are read, which is why the
     * IDCT below is handed no table of its own.
     */
    for (i = 0; i < 64; i++) { quant_y[i] = 1; quant_c[i] = 1; }
    if (format & 0x8000000) {
        if (p + 0x80 > chunk_end) { fail(err, errlen, "a CMV chunk's quantisation tables lie outside it"); return 0; }
        for (i = 0; i < 64; i++) {
            quant_y[i] = (int16_t) d[p + jbp_zigzag_dc[i]];
            quant_c[i] = (int16_t) d[p + 0x40 + jbp_zigzag_dc[i]];
        }
        p += 0x80;
    }

    /* 0x005193eb: the tile grid, then how many tiles the table holds, then one
     * u16 per tile, the whole padded to a four-byte boundary. */
    if (p + 4 > chunk_end) { fail(err, errlen, "a CMV chunk's tile table lies outside it"); return 0; }
    tiles = u16v(d, p);
    tile_count = u16v(d, p + 2);
    tile_table = p + 4;
    if ((tiles & 0xFF) * (tiles >> 8) != tile_count
        || tile_count != m->tiles_x * m->tiles_y) {
        fail(err, errlen, "a CMV chunk's tile grid does not match the picture");
        return 0;
    }
    p += (2 * tile_count + 7) & ~3;
    if (p > chunk_end) { fail(err, errlen, "a CMV chunk's tile table lies outside it"); return 0; }

    if (!is_intra && (format & 0x1000)) {
        /*
         * The per-frame mask. A CMV9 writes a u16 length in front of it
         * (0x00519418); a CMV5..CMV8 does not, and its length is the grid's own
         * bits rounded up to a four-byte boundary (0x00513c57). The block is
         * stepped over either way, because the bitstreams start after it; what
         * `allow_delta` decides is only whether it is CONSULTED, which is
         * 0x0051942a and 0x00513c74 - a frame that does not follow the one last
         * drawn may not leave any of its macroblocks holding old pixels.
         */
        int len, at;
        if (m->mask >= 0) {
            if (p + 2 > chunk_end) { fail(err, errlen, "a CMV frame mask lies outside the chunk"); return 0; }
            len = u16v(d, p);
            at = p + 2;
            p += len + 2;
        } else {
            len = (((m->mb_w * m->mb_h + 7) >> 3) + 3) & ~3;
            at = p;
            p += len;
        }
        if (p > chunk_end || len <= 0) {
            fail(err, errlen, "a CMV frame mask lies outside the chunk");
            return 0;
        }
        if (allow_delta) { frame_mask = at; delta = 1; }
    }

    if ((long) p + dc_bits + ac_bits > chunk_end) {
        fail(err, errlen, "a CMV chunk's bit streams lie outside it");
        return 0;
    }
    /*
     * The original reads its bits a dword at a time, so it may touch up to
     * three bytes past a stream's declared length; a well-formed file never
     * needs them, but the reader is given the same room so that a legal file is
     * never called short.
     */
    jbp_bits_init(&bits_dc, d, p, p + dc_bits + 3 <= m->size ? p + dc_bits + 3 : m->size);
    jbp_bits_init(&bits_ac, d, p + dc_bits,
                  p + dc_bits + ac_bits + 3 <= m->size ? p + dc_bits + ac_bits + 3 : m->size);

    if (dc_count > m->dc_room) {
        int16_t *grown = realloc(m->dc, (size_t) dc_count * sizeof *grown);
        if (!grown) { fail(err, errlen, "out of memory for a CMV chunk"); return 0; }
        m->dc = grown;
        m->dc_room = dc_count;
    }
    /* The DC run: one Huffman symbol saying how many bits the difference takes,
     * then the difference, accumulated in sixteen bits (0x00519490 and its
     * twin 0x00518f28, where the running sum is kept in dx and stored as a
     * word). */
    for (i = 0; i < dc_count; i++) {
        int n = jbp_huffman_read(&tree_dc, &bits_dc);
        int v = 0;
        if (n) {
            uint32_t raw = (uint32_t) jbp_bits_get(&bits_dc, n);
            uint32_t limit = 1u << ((unsigned) (n - 1) & 31);
            v = (int) raw;
            if (raw < limit) v -= (int) ((1u << ((unsigned) n & 31)) - 1u);
        }
        prev = (int) (int16_t) (uint16_t) (unsigned) (prev + v);
        m->dc[i] = (int16_t) prev;
        if (bits_dc.overrun) { fail(err, errlen, "a CMV DC stream ended early"); return 0; }
    }

    for (t = 0; t < tile_count; t++) {
        int ent = u16v(d, tile_table + 2 * t);
        int rows = ent & 0xFF, cols = ent >> 8;
        int tx = t % m->tiles_x, ty = t / m->tiles_x;
        int r, c;
        if (rows != tile_rows(m, ty) || cols != tile_cols(m, tx)) {
            fail(err, errlen, "a CMV tile is not the shape the picture gives it");
            return 0;
        }
        for (r = 0; r < rows; r++) {
            for (c = 0; c < cols; c++) {
                int16_t block[6][64];
                int bit, coded, write = 1, n;
                int px = tx * CMV_TILE + c * 16, py = ty * CMV_TILE + r * 16;

                if (mask_bit >= m->mb_w * m->mb_h) {
                    fail(err, errlen, "a CMV chunk asks for more macroblocks than the grid holds");
                    return 0;
                }
                /* With no movie block there is no static set: every macroblock
                 * of every frame is coded. */
                bit = m->mask >= 0
                    ? (d[m->mask + (mask_bit >> 3)] >> (7 - (mask_bit & 7))) & 1 : 0;
                mask_bit++;
                coded = (m->mask < 0) || (is_intra ? bit : !bit);
                if (!coded) continue;
                if (delta) {
                    if (frame_mask + (frame_bit >> 3) >= chunk_end) {
                        fail(err, errlen, "a CMV frame mask is shorter than its own frame");
                        return 0;
                    }
                    if ((d[frame_mask + (frame_bit >> 3)] >> (7 - (frame_bit & 7))) & 1) write = 0;
                    frame_bit++;
                }
                if (px + 16 > m->aligned_w || py + 16 > m->aligned_h) {
                    fail(err, errlen, "a CMV tile reaches outside the picture");
                    return 0;
                }

                memset(block, 0, sizeof block);
                for (n = 0; n < 6; n++) {
                    const int16_t *q = n < 4 ? quant_y : quant_c;
                    int k = 0;
                    if (dc_used >= dc_count) {
                        fail(err, errlen, "a CMV chunk wants more DC values than it declared");
                        return 0;
                    }
                    block[n][0] = (int16_t) (m->dc[dc_used++] * q[0]);
                    for (;;) {
                        int bit_count = jbp_huffman_read(&tree_ac, &bits_ac);
                        if (bits_ac.overrun) { fail(err, errlen, "a CMV AC stream ended early"); return 0; }
                        if (bit_count == 15) break;
                        if (bit_count == 0) {
                            int node = 0;
                            while (jbp_bits_get(&bits_ac, 1)) {
                                node++;
                                if (node >= JBP_RUN_TABLE || bits_ac.overrun) break;
                            }
                            if (node >= JBP_RUN_TABLE) { fail(err, errlen, "a bad CMV AC run"); return 0; }
                            k += run_table[node];
                        } else {
                            uint32_t raw = (uint32_t) jbp_bits_get(&bits_ac, bit_count);
                            uint32_t limit = 1u << ((unsigned) (bit_count - 1) & 31);
                            int v = (int) raw;
                            if (raw < limit) v -= (int) ((1u << ((unsigned) bit_count & 31)) - 1u);
                            block[n][jbp_zigzag[k]] = (int16_t) (v * q[k + 1]);
                            k++;
                        }
                        if (k >= 63) break;
                    }
                }
                if (!write) continue;
                for (n = 0; n < 6; n++) jbp_idct(block[n], NULL);
                jbp_macroblock(m->pixels, m->stride, py * m->stride + px * 4, block);
            }
        }
    }
    return 1;
}

int cmv_intra(cmv_movie *m, char *err, size_t errlen)
{
    if (!m) return 0;
    /* Only a CMV9 has one; 0x004319e2 runs 0x00431060 for '9' alone. */
    if (m->intra < 0) return 1;
    return decode_chunk(m, m->intra, m->base, 1, 0, err, errlen);
}

int cmv_frame(cmv_movie *m, int index, int follows, char *err, size_t errlen)
{
    int off, len;
    if (!m) return 0;
    if (index < 0 || index >= m->frames) { fail(err, errlen, "no such CMV frame"); return 0; }
    if (entry_codec(m, index) != 2) { fail(err, errlen, "a CMV frame is not a JBPD chunk"); return 0; }
    off = entry_offset(m, index);
    len = entry_size(m, index);
    if (off < 0 || len <= 0 || (long) m->base + off + len > m->size) {
        fail(err, errlen, "a CMV frame lies outside the file");
        return 0;
    }
    return decode_chunk(m, m->base + off, m->base + off + len, 0, follows, err, errlen);
}
