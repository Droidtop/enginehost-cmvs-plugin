#include "cmvs_md5.h"

#include <math.h>

static uint32_t sine_table[64];
static int sine_ready;

static const uint8_t shifts[4][4] = {
    {7, 12, 17, 22}, {5, 9, 14, 20}, {4, 11, 16, 23}, {6, 10, 15, 21},
};

static const uint32_t initial_state[CMVS_MD5_VARIANT_COUNT][4] = {
    /* A       */ {0xC74A2B01u, 0xE7C8AB8Fu, 0xD8BEDC4Eu, 0x7302A4C5u},
    /* B       */ {0x53FE9B2Cu, 0xF2C93EA8u, 0xEE81BA59u, 0xA2C8973Eu},
    /* CHRONO  */ {0xC74A2B01u, 0xE7C8AB8Fu, 0xD8BEDC4Eu, 0x7302A4C5u},
    /* MEMORIA */ {0xA79463F9u, 0xB6E755C5u, 0xC696AF21u, 0x6983E978u},
    /* NATSU   */ {0x63FE9A7Cu, 0xC2B93E98u, 0xEF91BA5Cu, 0x72C9A82Eu},
    /* AOI     */ {0xC74A2B02u, 0xE7C8AB8Fu, 0x38BEBC4Eu, 0x7531A4C3u},
    /* MIRAI   */ {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u},
};

static uint32_t rotl32(uint32_t v, int n)
{
    return (v << n) | (v >> (32 - n));
}

static void init_sine(void)
{
    int i;
    if (sine_ready) return;
    for (i = 0; i < 64; i++) {
        /* The reference casts the scaled sine through a signed 64-bit value
         * before truncating, so do the same rather than going via uint64. */
        sine_table[i] = (uint32_t) (int64_t) (fabs(sin((double) (i + 1))) * 4294967296.0);
    }
    sine_ready = 1;
}

static void transform(uint32_t state[4], const uint32_t buffer[16])
{
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    int i;
    for (i = 0; i < 64; i++) {
        uint32_t f, t;
        int g;
        if (i < 16) {
            f = d ^ (b & (c ^ d));
            g = i;
        } else if (i < 32) {
            f = c ^ (d & (b ^ c));
            g = (5 * i + 1) & 0xF;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) & 0xF;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) & 0xF;
        }
        t = d;
        d = c;
        c = b;
        b += rotl32(a + f + buffer[g] + sine_table[i], shifts[i >> 4][i & 3]);
        a = t;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

void cmvs_md5(cmvs_md5_variant variant, const uint32_t src[4], uint32_t out[4])
{
    uint32_t state[4];
    uint32_t buffer[16] = {0};
    int i;

    if (variant < 0 || variant >= CMVS_MD5_VARIANT_COUNT) variant = CMVS_MD5_MIRAI;
    init_sine();
    for (i = 0; i < 4; i++) {
        state[i] = initial_state[variant][i];
        buffer[i] = src[i];
    }
    buffer[4] = 0x80;
    buffer[14] = 0x80;
    transform(state, buffer);

    switch (variant) {
    case CMVS_MD5_A:
        out[0] = state[3]; out[1] = state[1]; out[2] = state[2]; out[3] = state[0];
        break;
    case CMVS_MD5_CHRONO:
        out[0] = state[2] ^ 0x45A76C2Fu;
        out[1] = state[1] - 0x5BA17FCBu;
        out[2] = state[0] ^ 0x79ABE8ADu;
        out[3] = state[3] - 0x1C08561Bu;
        break;
    case CMVS_MD5_B:
        out[0] = state[1] ^ 0x49875325u;
        out[1] = state[2] + 0x54F46D7Du;
        out[2] = state[3] ^ 0xAD7948B7u;
        out[3] = state[0] + 0x1D0638ADu;
        break;
    case CMVS_MD5_MEMORIA:
        out[0] = state[1]; out[1] = state[2]; out[2] = state[3]; out[3] = state[0];
        break;
    case CMVS_MD5_NATSU:
        out[0] = state[1] + 0x45876329u;
        out[1] = state[2] ^ 0x54F36D6Cu;
        out[2] = state[3] + 0x4387A749u;
        out[3] = state[0] ^ 0xE3F9A742u;
        break;
    case CMVS_MD5_AOI:
        out[0] = state[2] ^ 0x53A76D2Eu;
        out[1] = state[1] + 0x5BB17FDAu;
        out[2] = state[0] + 0x6853E14Du;
        out[3] = state[3] ^ 0xF5C6A9A3u;
        break;
    default:
        out[0] = state[0]; out[1] = state[1]; out[2] = state[2]; out[3] = state[3];
        break;
    }
}
