/*
 * A plain MD5, used only to check a CPZ index against the digest stored in the
 * archive header. This is a real MD5; the mangled per-game one lives in
 * cmvs_md5.c and is a different thing entirely.
 */
#ifndef CMVS_PLAIN_MD5_H
#define CMVS_PLAIN_MD5_H

#include <stddef.h>
#include <stdint.h>

void cmvs_plain_md5(const uint8_t *data, size_t length, uint8_t digest[16]);

#endif
