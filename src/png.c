#include "png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

static uint32_t crc_of(const uint8_t *p, size_t n)
{
    return (uint32_t) crc32(crc32(0L, NULL, 0), p, (uInt) n);
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t) (v >> 24);
    p[1] = (uint8_t) (v >> 16);
    p[2] = (uint8_t) (v >> 8);
    p[3] = (uint8_t) v;
}

static int chunk(FILE *f, const char *type, const uint8_t *data, size_t len)
{
    uint8_t head[8], tail[4];
    uint32_t crc;
    uint8_t *joined;

    be32(head, (uint32_t) len);
    memcpy(head + 4, type, 4);
    joined = malloc(len + 4);
    if (!joined) return 0;
    memcpy(joined, type, 4);
    if (len) memcpy(joined + 4, data, len);
    crc = crc_of(joined, len + 4);
    free(joined);

    be32(tail, crc);
    if (fwrite(head, 1, 8, f) != 8) return 0;
    if (len && fwrite(data, 1, len, f) != len) return 0;
    return fwrite(tail, 1, 4, f) == 4;
}

int cmvs_png_write(const char *path, const uint8_t *pixels, int width, int height,
                   char *err, size_t errlen)
{
    static const uint8_t SIGNATURE[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    FILE *f = NULL;
    uint8_t header[13];
    uint8_t *raw = NULL, *packed = NULL;
    uLongf packed_size;
    size_t raw_size;
    int row, col, ok = 0;

    if (width <= 0 || height <= 0 || !pixels) {
        if (err && errlen) snprintf(err, errlen, "nothing to write");
        return 0;
    }
    /* One filter byte per row, then RGBA: PNG is the other way round from the
     * BGRA the image codec and the scene both work in. */
    raw_size = (size_t) height * (1 + (size_t) width * 4);
    raw = malloc(raw_size);
    if (!raw) { if (err && errlen) snprintf(err, errlen, "out of memory"); return 0; }
    for (row = 0; row < height; row++) {
        uint8_t *out = raw + (size_t) row * (1 + (size_t) width * 4);
        const uint8_t *in = pixels + (size_t) row * width * 4;
        *out++ = 0;
        for (col = 0; col < width; col++) {
            out[0] = in[2];
            out[1] = in[1];
            out[2] = in[0];
            out[3] = in[3];
            out += 4;
            in += 4;
        }
    }

    packed_size = compressBound((uLong) raw_size);
    packed = malloc(packed_size);
    if (!packed || compress2(packed, &packed_size, raw, (uLong) raw_size, 6) != Z_OK) {
        if (err && errlen) snprintf(err, errlen, "deflate failed");
        goto done;
    }

    f = fopen(path, "wb");
    if (!f) { if (err && errlen) snprintf(err, errlen, "cannot write %s", path); goto done; }
    if (fwrite(SIGNATURE, 1, 8, f) != 8) goto done;

    be32(header, (uint32_t) width);
    be32(header + 4, (uint32_t) height);
    header[8] = 8;    /* bit depth */
    header[9] = 6;    /* colour type: RGBA */
    header[10] = 0;   /* deflate */
    header[11] = 0;   /* no filtering beyond the per-row byte */
    header[12] = 0;   /* not interlaced */
    if (!chunk(f, "IHDR", header, sizeof header)) goto done;
    if (!chunk(f, "IDAT", packed, packed_size)) goto done;
    if (!chunk(f, "IEND", NULL, 0)) goto done;
    ok = 1;

done:
    if (f) fclose(f);
    free(raw);
    free(packed);
    if (!ok && err && errlen && !*err) snprintf(err, errlen, "write failed");
    return ok;
}
