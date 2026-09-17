/*
 * The "JBP1" transform codec a PB3 type 3 carries, transcribed from 0x0051a080.
 *
 * It is a JPEG in everything but its container: two 16-entry Huffman trees
 * built from frequency tables rather than code lengths, a 16-byte run table,
 * the usual zigzag, an integer IDCT and a fixed-point YCbCr conversion.
 *
 * The same codec is what a .cmv movie is made of - the chunks there say "JBPD"
 * and wrap the same tables differently - so the transform, the trees, the
 * bitstream and the macroblock writer are shared with cmv.c rather than
 * transcribed twice. Those four are the only reason this header exposes
 * anything beyond jbp_decode.
 */
#ifndef CMVS_JBP_H
#define CMVS_JBP_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *pixels;   /* BGRA, top-down; caller frees */
    int stride;        /* 4 * aligned width */
    int size;
} jbp_result;

/* Decodes the JBP1 at `offset` in `data`. Returns 1 on success. */
int jbp_decode(const uint8_t *data, int size, int offset, jbp_result *out,
               char *err, size_t errlen);

/* ------------------------------------------------- shared with cmv.c ------ */

/*
 * The bitstream. Bits come out of the bytes least-significant first, which is
 * the same order as the original's dword reader (0x0050c6b0 takes bit `n` of
 * the little-endian dword at its cursor); a multi-bit value takes the first
 * bit read as its most significant.
 */
typedef struct {
    const uint8_t *in;
    int pos, end;
    uint32_t bits;
    int cached;
    int overrun;
} jbp_bits;

void jbp_bits_init(jbp_bits *s, const uint8_t *data, int pos, int end);
int jbp_bits_get(jbp_bits *s, int count);

/* One of the two Huffman trees. 0x0050c490 and 0x00519af0 build the same one:
 * the two lowest frequencies are joined until none is left, so the tree is a
 * plain Huffman over 16 leaves. `freq` is consumed. */
typedef struct {
    int leaf_count;
    int nodes[0x400];
    int root;
} jbp_huffman;

void jbp_huffman_build(jbp_huffman *h, int leaf_count, int32_t *freq);
int jbp_huffman_read(const jbp_huffman *h, jbp_bits *s);

/* The run table both decoders keep: sixteen bytes, each one more than the byte
 * in the file (0x005192f0, 0x00518dd0). */
#define JBP_RUN_TABLE 0x10

/* The zigzag an AC coefficient's index walks, and the same with the DC
 * position in front of it, which is the order a quantisation table is stored
 * in (0x00568140 and 0x005681c0). */
extern const uint8_t jbp_zigzag[64];
extern const uint8_t jbp_zigzag_dc[64];

/* The transform, 0x00519c50 and its SSE twin 0x00545140. `q` may be NULL when
 * the coefficients have already been multiplied by their quantisation values,
 * which is what the movie decoder does. */
void jbp_idct(int16_t *t, const int16_t *q);

/*
 * One 16x16 macroblock of six transformed blocks - four luma, then Cb, then Cr
 * - written as BGRA at `origin` bytes into `pixels` (0x0050e190 and the writer
 * inside 0x0051a080, which agree down to the clamp table's bias).
 */
void jbp_macroblock(uint8_t *pixels, int stride, int origin, int16_t block[6][64]);

#endif
