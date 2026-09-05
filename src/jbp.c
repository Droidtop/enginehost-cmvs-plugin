#include "jbp.h"

#include <stdlib.h>
#include <string.h>

#define MAX_FREQ 2100000000

static const uint8_t ZIGZAG[64] = {
     1,  8, 16,  9,  2,  3, 10, 17,
    24, 32, 25, 18, 11,  4,  5, 12,
    19, 26, 33, 40, 48, 41, 34, 27,
    20, 13,  6,  7, 14, 21, 28, 35,
    42, 49, 56, 57, 50, 43, 36, 29,
    22, 15, 23, 30, 37, 44, 51, 58,
    59, 52, 45, 38, 31, 39, 46, 53,
    60, 61, 54, 47, 55, 62, 63,  0,
};

static uint8_t REVERSE[256];
static int reverse_ready;

static void init_reverse(void)
{
    int i;
    if (reverse_ready) return;
    for (i = 0; i < 256; i++) {
        int x = ((i & 0xAA) >> 1) | ((i & 0x55) << 1);
        x = ((x & 0xCC) >> 2) | ((x & 0x33) << 2);
        REVERSE[i] = (uint8_t) (((x >> 4) | (x << 4)) & 0xFF);
    }
    reverse_ready = 1;
}

static void fail(char *err, size_t errlen, const char *msg)
{
    if (err && errlen) { strncpy(err, msg, errlen - 1); err[errlen - 1] = 0; }
}

static int32_t i32(const uint8_t *b, size_t o)
{
    return (int32_t) ((uint32_t) b[o] | ((uint32_t) b[o + 1] << 8)
                    | ((uint32_t) b[o + 2] << 16) | ((uint32_t) b[o + 3] << 24));
}

static int u16(const uint8_t *b, size_t o) { return b[o] | (b[o + 1] << 8); }

/* ---------------------------------------------------------------- bitstream */

typedef struct {
    const uint8_t *in;
    int pos, end;
    uint32_t bits;
    int cached;
    int overrun;
} bitstream;

static int get_bits(bitstream *s, int count)
{
    while (s->cached < count) {
        if (s->pos >= s->end) { s->overrun = 1; return 0; }
        s->bits = (s->bits << 8) | REVERSE[s->in[s->pos++]];
        s->cached += 8;
    }
    s->cached -= count;
    return (int) ((s->bits >> s->cached) & ((1u << count) - 1u));
}

/*
 * The reference reads a coefficient's sign on 32-bit unsigned values, where a
 * DC bit count of 0 - a legal "unchanged" code - makes the shift count wrap to
 * 31 and the correction term vanish. C's shift by a negative count is undefined
 * rather than masked, so mask it explicitly and compare unsigned.
 */
static int signed_coeff(int v, int bit_count)
{
    uint32_t limit = 1u << ((unsigned) (bit_count - 1) & 31);
    if ((uint32_t) v < limit) v -= (int) ((1u << ((unsigned) bit_count & 31)) - 1u);
    return v;
}

/* ------------------------------------------------------------------ huffman */

typedef struct {
    const uint8_t *base;
    int leaf_count;
    int nodes[0x400];
    int root;
} huffman;

static void huffman_build(huffman *h, const uint8_t *base, int leaf_count, int32_t *freq)
{
    int depth = leaf_count;
    h->base = base;
    h->leaf_count = leaf_count;
    memset(h->nodes, 0, sizeof h->nodes);
    for (;;) {
        int l = -1, r = -1, i;
        int32_t min = MAX_FREQ - 1;
        for (i = 0; i < depth; i++) if (freq[i] < min) { min = freq[i]; l = i; }
        min = MAX_FREQ - 1;
        for (i = 0; i < depth; i++) if (i != l && freq[i] < min) { min = freq[i]; r = i; }
        if (l < 0 || r < 0) break;
        h->nodes[depth] = l;
        h->nodes[depth + 0x200] = r;
        freq[depth++] = freq[l] + freq[r];
        freq[l] = MAX_FREQ;
        freq[r] = MAX_FREQ;
    }
    h->root = depth - 1;
}

static int huffman_read(const huffman *h, bitstream *s)
{
    int v = h->root;
    while (v >= h->leaf_count) {
        v = h->nodes[v + (get_bits(s, 1) << 9)];
        if (s->overrun) return 0;
    }
    return v;
}

/* --------------------------------------------------------------------- IDCT */

static int16_t s16(int v) { return (int16_t) (uint16_t) (unsigned) v; }

static void idct(int16_t *t, const int16_t *q)
{
    int p;
    for (p = 0; p < 8; p++) {
        if (t[p + 0x08] == 0 && t[p + 0x10] == 0 && t[p + 0x18] == 0 && t[p + 0x20] == 0
            && t[p + 0x28] == 0 && t[p + 0x30] == 0 && t[p + 0x38] == 0) {
            int16_t v = s16(t[p] * q[p]);
            t[p] = t[p + 0x08] = t[p + 0x10] = t[p + 0x18] = v;
            t[p + 0x20] = t[p + 0x28] = t[p + 0x30] = t[p + 0x38] = v;
        } else {
            int c = q[p + 0x10] * t[p + 0x10];
            int d = q[p + 0x30] * t[p + 0x30];
            int x = ((c + d) * 35467) >> 16;
            int a, b, w, yy, z, n, u, v, s, ra;
            c = ((c * 50159) >> 16) + x;
            d = ((d * -121094) >> 16) + x;
            a = t[p] * q[p];
            b = t[p + 0x20] * q[p + 0x20];
            w = a + b + c;
            x = a + b - c;
            yy = a - b + d;
            z = a - b - d;

            c = t[p + 0x38] * q[p + 0x38];
            d = t[p + 0x28] * q[p + 0x28];
            a = t[p + 0x18] * q[p + 0x18];
            b = t[p + 0x08] * q[p + 0x08];
            n = ((a + b + c + d) * 77062) >> 16;

            u  = n + ((c * 19571) >> 16)  + (((c + a) * -128553) >> 16) + (((c + b) * -58980) >> 16);
            v  = n + ((d * 134553) >> 16) + (((d + b) * -25570) >> 16)  + (((d + a) * -167963) >> 16);
            s  = n + ((b * 98390) >> 16)  + (((d + b) * -25570) >> 16)  + (((c + b) * -58980) >> 16);
            ra = n + ((a * 201373) >> 16) + (((c + a) * -128553) >> 16) + (((d + a) * -167963) >> 16);

            t[p]        = s16(w + s);
            t[p + 0x38] = s16(w - s);
            t[p + 0x08] = s16(yy + ra);
            t[p + 0x30] = s16(yy - ra);
            t[p + 0x10] = s16(z + v);
            t[p + 0x28] = s16(z - v);
            t[p + 0x18] = s16(x + u);
            t[p + 0x20] = s16(x - u);
        }
    }
    for (p = 0; p < 64; p += 8) {
        int a = t[p], c = t[p + 2], b = t[p + 4], d = t[p + 6];
        int x = ((c + d) * 35467) >> 16;
        int w, yy, z, n, s, ub, u, v;
        c = ((c * 50159) >> 16) + x;
        d = ((d * -121094) >> 16) + x;
        w = a + b + c;
        x = a + b - c;
        yy = a - b + d;
        z = a - b - d;

        d = t[p + 5];
        b = t[p + 1];
        c = t[p + 7];
        a = t[p + 3];
        n = ((a + b + c + d) * 77062) >> 16;

        s  = n + ((a * 201373) >> 16) + (((a + c) * -128553) >> 16) + (((a + d) * -167963) >> 16);
        ub = n + ((b * 98390) >> 16)  + (((b + c) * -58980) >> 16)  + (((b + d) * -25570) >> 16);
        u  = n + ((c * 19571) >> 16)  + (((b + c) * -58980) >> 16)  + (((a + c) * -128553) >> 16);
        v  = n + ((d * 134553) >> 16) + (((b + d) * -25570) >> 16)  + (((a + d) * -167963) >> 16);

        t[p]     = s16((w + ub) >> 3);
        t[p + 7] = s16((w - ub) >> 3);
        t[p + 1] = s16((yy + s) >> 3);
        t[p + 6] = s16((yy - s) >> 3);
        t[p + 2] = s16((z + v) >> 3);
        t[p + 5] = s16((z - v) >> 3);
        t[p + 3] = s16((x + u) >> 3);
        t[p + 4] = s16((x - u) >> 3);
    }
}

static uint8_t clamp_ycc(int c)
{
    if (c < 0x100) return 0;
    if (c >= 0x200) return 0xFF;
    return (uint8_t) (c - 0x100);
}

/* -------------------------------------------------------------------- decode */

int jbp_decode(const uint8_t *data, int size, int offset, jbp_result *out,
               char *err, size_t errlen)
{
    int data_pos, format, w, h, dc_bits, ac_bits;
    int aligned_w, aligned_h, blocks_x, blocks_y, stride, out_size;
    int tree_pos, quant_pos, bits_offset, total, i, x, y;
    uint8_t tree_data[0x10];
    int32_t freq[0x20];
    int16_t quant_y[0x40], quant_c[0x40], block[6][64];
    int16_t *dc = NULL;
    uint8_t *pixels = NULL;
    huffman tree_dc, tree_ac;
    bitstream bits_dc, bits_ac;
    int prev = 0;

    init_reverse();
    if (offset < 0 || offset + 0x24 > size) { fail(err, errlen, "JBP header lies outside the file"); return 0; }
    data_pos = i32(data, (size_t) offset + 4) + offset;
    format = i32(data, (size_t) offset + 8);
    w = u16(data, (size_t) offset + 0x10);
    h = u16(data, (size_t) offset + 0x12);
    dc_bits = i32(data, (size_t) offset + 0x1C);
    ac_bits = i32(data, (size_t) offset + 0x20);
    if (w <= 0 || h <= 0 || dc_bits < 0 || ac_bits < 0) { fail(err, errlen, "Bad JBP header"); return 0; }

    switch (((unsigned) format >> 28) & 3) {
    case 0: aligned_w = (w + 7) & ~7;       aligned_h = (h + 7) & ~7;       break;
    case 1: aligned_w = (w + 0xF) & ~0xF;   aligned_h = (h + 0xF) & ~0xF;   break;
    case 2: aligned_w = (w + 0x1F) & ~0x1F; aligned_h = (h + 0xF) & ~0xF;   break;
    default: fail(err, errlen, "Bad JBP alignment"); return 0;
    }
    blocks_x = aligned_w >> 4;
    blocks_y = aligned_h >> 4;
    stride = 4 * aligned_w;
    out_size = stride * aligned_h;

    tree_pos = data_pos + 0x80;
    quant_pos = tree_pos + 0x10;
    bits_offset = quant_pos + 0x80;
    if (data_pos < 0 || bits_offset < 0 || bits_offset > size
        || (long) bits_offset + dc_bits + ac_bits > size) {
        fail(err, errlen, "JBP tables or bit streams lie outside the file");
        return 0;
    }

    for (i = 0; i < 0x10; i++) tree_data[i] = (uint8_t) (data[tree_pos + i] + 1);
    memset(freq, 0, sizeof freq);
    for (i = 0; i < 16; i++) freq[i] = i32(data, (size_t) data_pos + 4 * (size_t) i);
    huffman_build(&tree_dc, tree_data, 0x10, freq);
    memset(freq, 0, sizeof freq);
    for (i = 0; i < 16; i++) freq[i] = i32(data, (size_t) data_pos + 0x40 + 4 * (size_t) i);
    huffman_build(&tree_ac, tree_data, 0x10, freq);

    memset(quant_y, 0, sizeof quant_y);
    memset(quant_c, 0, sizeof quant_c);
    if (format & 0x8000000) {
        for (i = 0; i < 0x40; i++) {
            quant_y[i] = (int16_t) data[quant_pos + i];
            quant_c[i] = (int16_t) data[quant_pos + i + 0x40];
        }
    }

    memset(&bits_dc, 0, sizeof bits_dc);
    bits_dc.in = data; bits_dc.pos = bits_offset; bits_dc.end = bits_offset + dc_bits;
    memset(&bits_ac, 0, sizeof bits_ac);
    bits_ac.in = data; bits_ac.pos = bits_offset + dc_bits; bits_ac.end = bits_offset + dc_bits + ac_bits;

    total = blocks_x * blocks_y;
    dc = calloc((size_t) total * 6 + 1, sizeof *dc);
    pixels = calloc(1, (size_t) out_size);
    if (!dc || !pixels) { fail(err, errlen, "Out of memory decoding a JBP image"); goto bad; }

    for (i = 0; i < total * 6; i++) {
        int n = huffman_read(&tree_dc, &bits_dc);
        prev += signed_coeff(get_bits(&bits_dc, n), n);
        dc[i] = (int16_t) prev;
        if (bits_dc.overrun) { fail(err, errlen, "JBP DC stream ended early"); goto bad; }
    }

    for (y = 0; y < blocks_y; y++) {
        int dst1 = y * stride * 16;
        int dst2 = dst1 + stride * 9;
        for (x = 0; x < blocks_x; x++) {
            int base = (y * blocks_x + x) * 6, n;
            memset(block, 0, sizeof block);
            for (n = 0; n < 6; n++) {
                int k = 0;
                block[n][0] = dc[base + n];
                while (k < 63) {
                    int bit_count = huffman_read(&tree_ac, &bits_ac);
                    if (bits_ac.overrun) { fail(err, errlen, "JBP AC stream ended early"); goto bad; }
                    if (bit_count == 15) break;
                    if (bit_count == 0) {
                        int node = 0;
                        while (get_bits(&bits_ac, 1)) {
                            node++;
                            if (node >= 0x10 || bits_ac.overrun) break;
                        }
                        if (node >= 0x10) { fail(err, errlen, "Bad JBP AC run"); goto bad; }
                        k += tree_data[node];
                    } else {
                        int v = signed_coeff(get_bits(&bits_ac, bit_count), bit_count);
                        block[n][ZIGZAG[k]] = s16(v);
                        k++;
                    }
                }
            }
            idct(block[0], quant_y);
            idct(block[1], quant_y);
            idct(block[2], quant_y);
            idct(block[3], quant_y);
            idct(block[4], quant_c);
            idct(block[5], quant_c);

            {
                static const int cbcr_base[4] = {0, 4, 32, 36};
                int pass;
                int dcs[4], acs[4];
                dcs[0] = dst1;               acs[0] = dst1 + stride;
                dcs[1] = dst1 + 32;          acs[1] = dst1 + stride + 32;
                dcs[2] = dst2 - stride;      acs[2] = dst2;
                dcs[3] = dst2 - stride + 32; acs[3] = dst2 + 32;
                for (pass = 0; pass < 4; pass++) {
                    const int16_t *dy = block[pass];
                    int dcp = dcs[pass], acp = acs[pass], cbcr = cbcr_base[pass];
                    int y_src = 0, row, col;
                    for (row = 0; row < 4; row++) {
                        for (col = 0; col < 4; col++) {
                            int cb = block[4][cbcr], cr = block[5][cbcr];
                            int r = (cr * 0x166F0) >> 16;
                            int g = ((cb * 0x5810) >> 16) + ((cr * 0xB6C0) >> 16);
                            int b = (cb * 0x1C590) >> 16;
                            int c0 = dy[y_src] + 0x180, c1 = dy[y_src + 1] + 0x180;
                            int c8 = dy[y_src + 8] + 0x180, c9 = dy[y_src + 9] + 0x180;
                            pixels[dcp]              = clamp_ycc(c0 + b);
                            pixels[acp + 1 - stride] = clamp_ycc(c0 - g);
                            pixels[acp + 2 - stride] = clamp_ycc(c0 + r);
                            pixels[acp + 4 - stride] = clamp_ycc(c1 + b);
                            pixels[acp + 5 - stride] = clamp_ycc(c1 - g);
                            pixels[acp + 6 - stride] = clamp_ycc(c1 + r);
                            pixels[acp]              = clamp_ycc(c8 + b);
                            pixels[acp + 1]          = clamp_ycc(c8 - g);
                            pixels[acp + 2]          = clamp_ycc(c8 + r);
                            pixels[acp + 4]          = clamp_ycc(c9 + b);
                            pixels[acp + 5]          = clamp_ycc(c9 - g);
                            pixels[acp + 6]          = clamp_ycc(c9 + r);
                            y_src += 2;
                            dcp += 8;
                            acp += 8;
                            cbcr++;
                        }
                        dcp += stride * 2 - 32;
                        acp += stride * 2 - 32;
                        y_src += 8;
                        cbcr += 4;
                    }
                }
            }
            dst1 += 64;
            dst2 += 64;
        }
    }

    free(dc);
    out->pixels = pixels;
    out->stride = stride;
    out->size = out_size;
    return 1;
bad:
    free(dc);
    free(pixels);
    return 0;
}
