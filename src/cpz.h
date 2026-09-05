/*
 * Reader for CMVS "CPZ" resource archives, which is how a real CMVS game ships
 * everything: scripts, images, sound.
 *
 * Only CPZ5 and CPZ6 are handled. CPZ7 adds a Huffman-packed index key and a
 * per-archive key lifted out of start.ps3, neither tested against a real game
 * here, so it is refused by name rather than half-implemented.
 *
 * Ported from morkt/GARbro's ArcFormats/Cmvs (MIT). Every length is checked
 * before use: these archives are game data, and a truncated or hostile one must
 * fail rather than read outside its buffer.
 */
#ifndef CMVS_CPZ_H
#define CMVS_CPZ_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "scheme.h"

typedef struct {
    char *name;        /* cp932 bytes, '/' separated, NUL terminated */
    char *lookup;      /* lowercased copy used for find() */
    long long offset;
    int size;
    uint32_t key;
} cpz_entry;

typedef struct cpz_archive cpz_archive;

/*
 * Opens the archive, trying each known scheme until one produces a sane index.
 * Returns NULL and fills err (if given) with a sentence saying why.
 */
cpz_archive *cpz_open(const char *path, char *err, size_t errlen);
void cpz_close(cpz_archive *a);

const char *cpz_scheme_name(const cpz_archive *a);
int cpz_count(const cpz_archive *a);
const cpz_entry *cpz_at(const cpz_archive *a, int index);
const cpz_entry *cpz_find(const cpz_archive *a, const char *name);

/*
 * Reads one entry, decrypted, and unpacked if it is a PS2A container. A PB3B
 * image gets its own header pass instead: CPZ masks bytes 8..0x33 of every
 * stored PB3, so without it the fields an image reader needs are still noise.
 *
 * The caller owns the returned buffer and frees it. *size_out gets its length.
 */
uint8_t *cpz_read(cpz_archive *a, const cpz_entry *e, int *size_out, char *err, size_t errlen);

/* Exposed because loose scripts on disk need it too. Frees nothing; returns a
 * new buffer holding the 0x30-byte header followed by the expanded body. */
uint8_t *cmvs_unpack_ps2(const uint8_t *data, int size, int *size_out, char *err, size_t errlen);

/* Unmasks a stored PB3B header in place. */
void cmvs_decrypt_pb3(uint8_t *data, int size);

#endif
