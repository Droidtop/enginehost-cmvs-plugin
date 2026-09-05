/*
 * The per-game constants a CPZ archive is encrypted with.
 *
 * They live in the game's own executable, not in the archive, so an archive
 * cannot say which set it needs: the reader tries each known scheme and keeps
 * the one whose index decrypts into a directory table that makes sense.
 */
#ifndef CMVS_SCHEME_H
#define CMVS_SCHEME_H

#include <stdint.h>

#include "cmvs_md5.h"

typedef struct {
    const char *name;
    cmvs_md5_variant md5_variant;
    const uint32_t *secret;      /* 24 words */
    uint32_t decoder_factor;
    uint32_t entry_init_key;
    uint32_t entry_sub_key;
    uint8_t entry_tail_key;
    int entry_key_pos;
    uint32_t index_seed;
    uint32_t index_addend;
    uint8_t index_subtrahend;
    uint32_t dir_key_addend[4];
} cmvs_scheme;

/*
 * The 24-word obfuscation table: a cp932 sentence the developers left in the
 * executable. Most CMVS games ship this same one and vary the scalars instead.
 */
extern const uint32_t cmvs_common_secret[24];

/* Every scheme the reader knows, most likely first. */
extern const cmvs_scheme cmvs_schemes[];
extern const int cmvs_scheme_count;

#endif
