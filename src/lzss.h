/*
 * The LZSS the save files are packed with.
 *
 * Both containers use Okumura's LZSS - a ring buffer, a flag byte whose eight
 * bits say literal or match, and a match encoded as two bytes - but with
 * different parameters, so the parameters are a value here rather than a
 * #define. CSV2 (the slot save, compressor 0x0045b100) has a 2048-byte ring
 * and matches up to 33; CSS1 (system.dat, compressor 0x0047a9b0) has 1024 and
 * 48. Nothing else differs, which is why one codec serves both.
 *
 * THRESHOLD is 1 in both: a two-byte match is worth encoding, and the stored
 * length is len - 2.
 */
#ifndef CMVS_LZSS_H
#define CMVS_LZSS_H

#include <stdint.h>

typedef struct {
    int n;              /* ring buffer size, a power of two */
    int f;              /* longest match */
    int pos_shift;      /* how far b1's high bits sit above the low byte */
    unsigned pos_high;  /* which bits of b1 those are */
    unsigned len_mask;  /* what is left of b1 for the length */
} cmvs_lzss;

/* pos = b0 | ((b1 & pos_high) << pos_shift), len = (b1 & len_mask) + 2. */
extern const cmvs_lzss cmvs_lzss_csv2;
extern const cmvs_lzss cmvs_lzss_css1;

/*
 * Expands into a caller's buffer and answers how many bytes it produced, or -1
 * if the stream asked to write past the end. The header carries the expanded
 * size, so the caller always knows how big the buffer has to be.
 */
int cmvs_lzss_expand(const cmvs_lzss *p, const uint8_t *in, int in_size,
                     uint8_t *out, int out_size);

/*
 * Compresses, returning a malloc'd buffer the caller frees. This is the
 * original's own greedy binary-tree search, not merely a compatible one: it
 * reproduces the reference saves' streams byte for byte, which is the only
 * reason a save of ours can be byte-compatible rather than just loadable.
 */
uint8_t *cmvs_lzss_compress(const cmvs_lzss *p, const uint8_t *in, int in_size,
                            int *out_size);

#endif
