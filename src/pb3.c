#include "pb3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BASE_DEPTH 4

static uint16_t u16(const uint8_t *b, size_t o)
{
    return (uint16_t) (b[o] | (b[o + 1] << 8));
}

static int32_t i32(const uint8_t *b, size_t o)
{
    return (int32_t) ((uint32_t) b[o] | ((uint32_t) b[o + 1] << 8)
                    | ((uint32_t) b[o + 2] << 16) | ((uint32_t) b[o + 3] << 24));
}

static void fail(char *err, size_t errlen, const char *msg)
{
    if (err && errlen) { strncpy(err, msg, errlen - 1); err[errlen - 1] = 0; }
}

/* ------------------------------------------------------------------- reader */

typedef struct {
    const uint8_t *in;
    int in_size;
    pb3_base_loader loader;
    void *ctx;
    int depth;

    int type, sub_type, width, height, bpp, channels, stride;
    int has_alpha;
    uint8_t *out;
    int out_size;
    uint8_t frame[0x800];

    char *err;
    size_t errlen;
} reader;

/* Every offset the file itself supplies goes through this. */
static int bounds(reader *r, const uint8_t *buf, int buf_size, long offset, long length,
                  const char *what)
{
    (void) buf;
    if (offset < 0 || length < 0 || offset + length > buf_size) {
        char msg[128];
        snprintf(msg, sizeof msg, "PB3 %s lies outside the file", what);
        fail(r->err, r->errlen, msg);
        return 0;
    }
    return 1;
}

static void reset_frame(reader *r)
{
    memset(r->frame, 0, 0x7DE);
}

static int lzss_unpack(reader *r, int bit_src, int data_src, uint8_t *out, int out_size)
{
    int dst = 0, bit_mask = 0x80, fp = 0x7DE;
    while (dst < out_size) {
        if (0 == bit_mask) { bit_mask = 0x80; bit_src++; }
        if (!bounds(r, r->in, r->in_size, bit_src, 1, "LZSS control")) return 0;
        if (r->in[bit_src] & bit_mask) {
            int v, count, offset, i;
            if (!bounds(r, r->in, r->in_size, data_src, 2, "LZSS match")) return 0;
            v = u16(r->in, (size_t) data_src);
            data_src += 2;
            count = (v & 0x1F) + 3;
            offset = v >> 5;
            for (i = 0; i < count && dst < out_size; i++) {
                uint8_t b = r->frame[(i + offset) & 0x7FF];
                out[dst++] = b;
                r->frame[fp] = b;
                fp = (fp + 1) & 0x7FF;
            }
        } else {
            uint8_t b;
            if (!bounds(r, r->in, r->in_size, data_src, 1, "LZSS literal")) return 0;
            b = r->in[data_src++];
            out[dst++] = b;
            r->frame[fp] = b;
            fp = (fp + 1) & 0x7FF;
        }
        bit_mask >>= 1;
    }
    return 1;
}

/* ------------------------------------------------------- type 1: block planes */

static int unpack_v1(reader *r)
{
    int x_blocks = (r->width + 15) >> 4;
    int y_blocks = (r->height + 15) >> 4;
    uint8_t *plane;
    int data1, data2, channel;

    r->out = calloc(1, (size_t) r->out_size);
    plane = calloc(1, (size_t) r->width * r->height);
    if (!r->out || !plane) { fail(r->err, r->errlen, "Out of memory decoding a PB3 image"); free(plane); return 0; }

    data1 = i32(r->in, 0x2C);
    data2 = i32(r->in, 0x30);

    for (channel = 0; channel < r->channels; channel++) {
        int channel_offset = 4 * r->channels, i, head, bit_src, channel_size, data_src;
        int plane_src = 0, bit_mask = 0x80, bottom = 16, y;
        for (i = 0; i < channel; i++) {
            if (!bounds(r, r->in, r->in_size, data1 + 4 * i, 4, "type 1 channel table")) goto bad;
            channel_offset += i32(r->in, (size_t) data1 + 4 * (size_t) i);
        }
        head = data1 + channel_offset;
        if (!bounds(r, r->in, r->in_size, head, 12, "type 1 channel header")) goto bad;
        bit_src = head + 12 + i32(r->in, (size_t) head) + i32(r->in, (size_t) head + 4);
        channel_size = i32(r->in, (size_t) head + 8);
        if (channel_size < 0 || channel_size > r->width * r->height) {
            fail(r->err, r->errlen, "PB3 type 1 channel size is out of range");
            goto bad;
        }

        channel_offset = 4 * r->channels;
        for (i = 0; i < channel; i++) {
            if (!bounds(r, r->in, r->in_size, data2 + 4 * i, 4, "type 1 channel table")) goto bad;
            channel_offset += i32(r->in, (size_t) data2 + 4 * (size_t) i);
        }
        data_src = data2 + channel_offset;

        reset_frame(r);
        if (!lzss_unpack(r, bit_src, data_src, plane, channel_size)) goto bad;

        bit_src = head + 12;
        data_src = bit_src + i32(r->in, (size_t) head);
        for (y = 0; y < y_blocks; y++) {
            int row = 16 * y, right = 16, dst_origin = r->stride * row + channel, x;
            for (x = 0; x < x_blocks; x++) {
                int dst = dst_origin;
                int bw = right > r->width ? r->width - 16 * x : 16;
                int bh = bottom > r->height ? r->height - row : 16;
                int j;
                if (0 == bit_mask) { bit_src++; bit_mask = 0x80; }
                if (!bounds(r, r->in, r->in_size, bit_src, 1, "type 1 block control")) goto bad;
                if (r->in[bit_src] & bit_mask) {
                    uint8_t b;
                    if (!bounds(r, r->in, r->in_size, data_src, 1, "type 1 flat block")) goto bad;
                    b = r->in[data_src++];
                    for (j = 0; j < bh; j++) {
                        int p = dst, k;
                        for (k = 0; k < bw; k++) { r->out[p] = b; p += 4; }
                        dst += r->stride;
                    }
                } else {
                    for (j = 0; j < bh; j++) {
                        int p = dst, k;
                        for (k = 0; k < bw; k++) { r->out[p] = plane[plane_src++]; p += 4; }
                        dst += r->stride;
                    }
                }
                bit_mask >>= 1;
                right += 16;
                dst_origin += 64;
            }
            bottom += 16;
        }
    }
    free(plane);
    return 1;
bad:
    free(plane);
    return 0;
}

/* --------------------------------------------------- type 5: delta LZSS planes */

static int unpack_v5(reader *r)
{
    int i;
    r->out = calloc(1, (size_t) r->out_size);
    if (!r->out) { fail(r->err, r->errlen, "Out of memory decoding a PB3 image"); return 0; }
    for (i = 0; i < 4; i++) {
        int bit_src, data_src, fp = 0x7DE, accum = 0, bit_mask = 0x80, dst = i;
        if (!bounds(r, r->in, r->in_size, 8 * i + 0x34, 8, "type 5 section table")) return 0;
        bit_src = 0x54 + i32(r->in, (size_t) (8 * i + 0x34));
        data_src = 0x54 + i32(r->in, (size_t) (8 * i + 0x38));
        reset_frame(r);
        while (dst < r->out_size) {
            if (0 == bit_mask) { bit_src++; bit_mask = 0x80; }
            if (!bounds(r, r->in, r->in_size, bit_src, 1, "type 5 control")) return 0;
            if (r->in[bit_src] & bit_mask) {
                int v, count, offset, k;
                if (!bounds(r, r->in, r->in_size, data_src, 2, "type 5 match")) return 0;
                v = u16(r->in, (size_t) data_src);
                data_src += 2;
                count = (v & 0x1F) + 3;
                offset = v >> 5;
                for (k = 0; k < count && dst < r->out_size; k++) {
                    uint8_t b = r->frame[(k + offset) & 0x7FF];
                    r->frame[fp] = b;
                    fp = (fp + 1) & 0x7FF;
                    accum = (accum + b) & 0xFF;
                    r->out[dst] = (uint8_t) accum;
                    dst += 4;
                }
            } else {
                uint8_t b;
                if (!bounds(r, r->in, r->in_size, data_src, 1, "type 5 literal")) return 0;
                b = r->in[data_src++];
                r->frame[fp] = b;
                fp = (fp + 1) & 0x7FF;
                accum = (accum + b) & 0xFF;
                r->out[dst] = (uint8_t) accum;
                dst += 4;
            }
            bit_mask >>= 1;
        }
    }
    return 1;
}

/* ------------------------------------------------- type 6/8: overlay on a base */

static const uint8_t NAME_KEY_V6[16] = {
    0xA6, 0x75, 0xF3, 0x9C, 0xC5, 0x69, 0x78, 0xA3,
    0x3E, 0xA5, 0x4F, 0x79, 0x59, 0xFE, 0x3A, 0xC7,
};

static void base_image_name(const reader *r, char *out, size_t outlen)
{
    size_t n = 0;
    int i;
    for (i = 0; i < 0x20 && n + 5 < outlen; i++) {
        int c = r->in[0x34 + i] ^ NAME_KEY_V6[i & 0xF];
        if (c == 0) break;
        out[n++] = (char) c;
    }
    snprintf(out + n, outlen - n, ".pb3");
}

static int decode_into(reader *r);

static int blend_overlay(reader *r)
{
    int bit_src = 0x20 + i32(r->in, 0xC);
    int data_src = bit_src + i32(r->in, 0x2C);
    int overlay_size = i32(r->in, 0x18);
    uint8_t *overlay;
    int o_bit_src = 8, o_data_src, bit_mask = 0x80;
    int x_blocks = (r->width + 7) >> 3;
    int y_blocks = (r->height + 7) >> 3;
    int h = 0, dst_origin = 0;

    if (overlay_size < 8 || overlay_size > 0x8000000) {
        fail(r->err, r->errlen, "PB3 overlay size is out of range");
        return 0;
    }
    overlay = calloc(1, (size_t) overlay_size);
    if (!overlay) { fail(r->err, r->errlen, "Out of memory decoding a PB3 overlay"); return 0; }
    reset_frame(r);
    if (!lzss_unpack(r, bit_src, data_src, overlay, overlay_size)) { free(overlay); return 0; }
    o_data_src = 8 + i32(overlay, 0);

    while (y_blocks > 0) {
        int w = 0, x;
        for (x = 0; x < x_blocks; x++) {
            if (0 == bit_mask) { o_bit_src++; bit_mask = 0x80; }
            if (!bounds(r, overlay, overlay_size, o_bit_src, 1, "overlay control")) { free(overlay); return 0; }
            if (0 == (overlay[o_bit_src] & bit_mask)) {
                int dst = 8 * (dst_origin + 4 * x);
                int x_count = r->width - w < 8 ? r->width - w : 8;
                int y_count = r->height - h < 8 ? r->height - h : 8;
                int j;
                for (j = 0; j < y_count; j++) {
                    int count = 4 * x_count;
                    if (!bounds(r, overlay, overlay_size, o_data_src, count, "overlay block")
                        || dst < 0 || dst + count > r->out_size) {
                        fail(r->err, r->errlen, "PB3 overlay block lies outside the image");
                        free(overlay);
                        return 0;
                    }
                    memcpy(r->out + dst, overlay + o_data_src, (size_t) count);
                    o_data_src += count;
                    dst += r->stride;
                }
            }
            bit_mask >>= 1;
            w += 8;
        }
        dst_origin += r->stride;
        h += 8;
        y_blocks--;
    }
    free(overlay);
    return 1;
}

static int unpack_v6(reader *r)
{
    char name[64];
    uint8_t *base = NULL;
    int base_size = 0;

    if (!r->loader) { fail(r->err, r->errlen, "PB3 overlay needs its base image"); return 0; }
    if (r->depth >= MAX_BASE_DEPTH) { fail(r->err, r->errlen, "PB3 base images nest too deeply"); return 0; }
    base_image_name(r, name, sizeof name);
    base = r->loader(r->ctx, name, &base_size);
    if (!base) { fail(r->err, r->errlen, "PB3 base image is missing"); return 0; }

    r->out = calloc(1, (size_t) r->out_size);
    if (!r->out) { free(base); fail(r->err, r->errlen, "Out of memory decoding a PB3 image"); return 0; }

    if (base_size > 4 && !memcmp(base, "PB3B", 4)) {
        reader sub = *r;
        sub.in = base;
        sub.in_size = base_size;
        sub.depth = r->depth + 1;
        sub.out = NULL;
        if (!decode_into(&sub)) { free(sub.out); free(base); return 0; }
        memcpy(r->out, sub.out, (size_t) (sub.out_size < r->out_size ? sub.out_size : r->out_size));
        free(sub.out);
    } else {
        memcpy(r->out, base, (size_t) (base_size < r->out_size ? base_size : r->out_size));
    }
    free(base);
    return blend_overlay(r);
}

/* -------------------------------------------------------- type 2/3: JBP1 codec */

#include "jbp.h"

static int unpack_jbp(reader *r, int jbp_pos, int alpha_pos)
{
    jbp_result jbp;
    if (!jbp_decode(r->in, r->in_size, jbp_pos, &jbp, r->err, r->errlen)) return 0;
    r->out = jbp.pixels;
    if (r->stride != jbp.stride) {
        int src = jbp.stride, dst = r->stride, y;
        for (y = 1; y < r->height; y++) {
            memmove(r->out + dst, r->out + src, (size_t) r->stride);
            src += jbp.stride;
            dst += r->stride;
        }
    }
    if (jbp.size < r->out_size) {
        fail(r->err, r->errlen, "JBP image is smaller than the PB3 header claims");
        return 0;
    }
    r->out_size = jbp.size;
    if (32 == r->bpp && alpha_pos > 0) {
        int dst = 3, end = r->stride * r->height;
        while (dst < end) {
            int alpha;
            if (!bounds(r, r->in, r->in_size, alpha_pos, 1, "alpha run")) return 0;
            alpha = r->in[alpha_pos++];
            if (alpha != 0 && alpha != 0xFF) {
                r->out[dst] = (uint8_t) alpha;
                dst += 4;
            } else {
                int count;
                if (!bounds(r, r->in, r->in_size, alpha_pos, 1, "alpha run length")) return 0;
                count = r->in[alpha_pos++];
                while (count-- > 0 && dst < end) {
                    r->out[dst] = (uint8_t) alpha;
                    dst += 4;
                }
            }
        }
    } else {
        r->has_alpha = 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ dispatch */

static int decode_into(reader *r)
{
    const uint8_t *d = r->in;
    if (r->in_size < 0x40 || memcmp(d, "PB3B", 4)) {
        fail(r->err, r->errlen, "Not a PB3B image");
        return 0;
    }
    r->sub_type = i32(d, 0x18);
    r->type = u16(d, 0x1C);
    r->width = u16(d, 0x1E);
    r->height = u16(d, 0x20);
    r->bpp = u16(d, 0x22);
    if (r->width <= 0 || r->height <= 0) { fail(r->err, r->errlen, "PB3 image has no extent"); return 0; }
    if (r->bpp != 8 && r->bpp != 24 && r->bpp != 32) {
        fail(r->err, r->errlen, "Unsupported PB3 bit depth");
        return 0;
    }
    if (r->type == 1 && r->sub_type != 0x10) {
        fail(r->err, r->errlen, "Unknown PB3 type 1 subtype");
        return 0;
    }
    r->channels = r->bpp / 8;
    r->stride = 4 * r->width;
    r->out_size = r->stride * r->height;
    r->has_alpha = r->channels >= 4;

    switch (r->type) {
    case 1: return unpack_v1(r);
    case 5: return unpack_v5(r);
    case 6: case 8: return unpack_v6(r);
    case 2: case 3: return unpack_jbp(r, 0x34, i32(d, 0x2C));
    default:
        {
            char msg[64];
            snprintf(msg, sizeof msg, "PB3 type %d images are not supported", r->type);
            fail(r->err, r->errlen, msg);
        }
        return 0;
    }
}

int pb3_decode(const uint8_t *data, int size, pb3_base_loader loader, void *ctx,
               pb3_image *out, char *err, size_t errlen)
{
    reader r;
    memset(&r, 0, sizeof r);
    r.in = data;
    r.in_size = size;
    r.loader = loader;
    r.ctx = ctx;
    r.err = err;
    r.errlen = errlen;
    if (!decode_into(&r)) { free(r.out); return 0; }
    out->width = r.width;
    out->height = r.height;
    out->has_alpha = r.has_alpha;
    out->pixels = r.out;
    return 1;
}

void pb3_free(pb3_image *img)
{
    if (!img) return;
    free(img->pixels);
    img->pixels = NULL;
}
