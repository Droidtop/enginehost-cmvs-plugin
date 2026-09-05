/*
 * "JBP1": the transform codec inside PB3 types 2 and 3.
 *
 * JPEG-shaped - 16x16 macroblocks of four luma and two chroma 8x8 blocks,
 * Huffman-coded DC deltas and AC runs, its own fixed-point IDCT - but with its
 * own bitstream (MSB-first over bit-reversed bytes) and its own Huffman tree
 * built from a frequency table rather than code lengths.
 */
#ifndef CMVS_JBP_H
#define CMVS_JBP_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *pixels;   /* BGRA at `stride`, caller frees */
    int stride;        /* 4 * aligned width, which can exceed the image width */
    int size;
} jbp_result;

int jbp_decode(const uint8_t *data, int size, int offset, jbp_result *out,
               char *err, size_t errlen);

#endif
