/*
 * CMVS's mangled MD5.
 *
 * The engine runs a single MD5 block over four header words and then permutes
 * the resulting state differently per game family, so the output is not an MD5
 * digest and no stock MD5 implementation produces it.
 */
#ifndef CMVS_MD5_H
#define CMVS_MD5_H

#include <stdint.h>

typedef enum {
    CMVS_MD5_A = 0,
    CMVS_MD5_B,
    CMVS_MD5_CHRONO,
    CMVS_MD5_MEMORIA,
    CMVS_MD5_NATSU,
    CMVS_MD5_AOI,
    CMVS_MD5_MIRAI,
    CMVS_MD5_VARIANT_COUNT
} cmvs_md5_variant;

/* Writes the four transformed words to out; src is left alone. */
void cmvs_md5(cmvs_md5_variant variant, const uint32_t src[4], uint32_t out[4]);

#endif
