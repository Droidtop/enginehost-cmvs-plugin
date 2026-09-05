/*
 * The PS2A script container: a 0x30-byte header, an index of dword entry
 * points, the bytecode, then a string pool the bytecode refers to by offset.
 */
#ifndef CMVS_SCRIPT_H
#define CMVS_SCRIPT_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *data;          /* the whole expanded container; owned */
    int size;

    const uint32_t *index;  /* entry points, into the bytecode */
    int index_count;

    const uint8_t *code;
    int code_size;

    const char *strings;    /* cp932, NUL separated */
    int strings_size;
} cmvs_script;

/* Takes ownership of `data` on success (a buffer from cpz_read). */
int cmvs_script_open(uint8_t *data, int size, cmvs_script *out, char *err, size_t errlen);
void cmvs_script_close(cmvs_script *s);

/* The string at a bytecode operand's offset, or NULL if it points nowhere. */
const char *cmvs_script_string(const cmvs_script *s, uint32_t offset);

#endif
