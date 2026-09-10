/*
 * The PS2A script container: a 0x30-byte header, an index of dword entry
 * points, the bytecode, the script's own variable area, then a string pool
 * the bytecode refers to by offset. 0x0046efdb lays all four out in order.
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

    /* Header 0x20: where the engine starts this script (0x46EF20 does
     * `mov eax,[eax+0x20]; mov [esi+0x3394],eax`). It is NOT index[0]:
     * main.ps3 starts at 0x377e8 and index[0] is 0, a different routine. */
    int entry;

    /*
     * Header 0x18: the script's own variable area, which lives BETWEEN the
     * code and the string pool - 0x46eff9 reads it and adds it to the end of
     * the code to find the pool. Three of ChronoClock's 84 scripts have one
     * (cdemo 16 bytes, intproc 356, omake 232) and every pool offset in those
     * three is past it, so a loader that ignores the field reads every one of
     * their strings from the wrong place.
     */
    int vars_size;

    const char *strings;    /* cp932, NUL separated */
    int strings_size;
} cmvs_script;

/* Takes ownership of `data` on success (a buffer from cpz_read). */
int cmvs_script_open(uint8_t *data, int size, cmvs_script *out, char *err, size_t errlen);
void cmvs_script_close(cmvs_script *s);

/* The string at a bytecode operand's offset, or NULL if it points nowhere. */
const char *cmvs_script_string(const cmvs_script *s, uint32_t offset);

#endif
