/*
 * Decoder for PB3B, the format every CMVS graphic is stored in.
 *
 * ChronoClock's 13,348 images use four of the format's variants: type 1 codes
 * each channel as 16x16 blocks over an LZSS plane, type 5 as four
 * delta-accumulated LZSS channels, type 3 with a JPEG-shaped "JBP1" transform
 * codec plus a run-length alpha channel, and type 6 patches 8x8 blocks onto a
 * named base image, which is how expression variants are stored without
 * repeating the whole portrait. Types 4 and 7 exist in the format but appear in
 * none of this game's archives, so they are refused rather than guessed at.
 *
 * Pixels come out BGRA, top-down, stride 4 * width, which is the layout the
 * format itself works in.
 */
#ifndef CMVS_PB3_H
#define CMVS_PB3_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int width;
    int height;
    int has_alpha;
    uint8_t *pixels;   /* BGRA, 4 * width * height bytes; caller frees */
} pb3_image;

/*
 * Supplies a sibling image by name for the type 6/8 overlay variants. The name
 * is cp932 with a ".pb3" suffix and no directory part; the loader decides where
 * to look. Must return a malloc'd buffer the decoder will free, or NULL.
 */
typedef uint8_t *(*pb3_base_loader)(void *ctx, const char *name, int *size_out);

/* Returns 1 on success. On failure fills err and leaves out untouched. */
int pb3_decode(const uint8_t *data, int size, pb3_base_loader loader, void *ctx,
               pb3_image *out, char *err, size_t errlen);

void pb3_free(pb3_image *img);

#endif
