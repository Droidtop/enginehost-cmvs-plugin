#include "script.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEADER 0x30

static uint32_t rd32(const uint8_t *d, size_t at)
{
    return (uint32_t) d[at] | ((uint32_t) d[at + 1] << 8)
         | ((uint32_t) d[at + 2] << 16) | ((uint32_t) d[at + 3] << 24);
}

static void fail(char *err, size_t errlen, const char *msg)
{
    if (err && errlen) { strncpy(err, msg, errlen - 1); err[errlen - 1] = 0; }
}

int cmvs_script_open(uint8_t *data, int size, cmvs_script *out, char *err, size_t errlen)
{
    int index_count, code_size, vars_size, strings_size, entry;
    long code_at, vars_at, strings_at;

    if (size < HEADER || memcmp(data, "PS2A", 4)) {
        fail(err, errlen, "Not a PS2A script");
        return 0;
    }
    if (rd32(data, 4) != HEADER) { fail(err, errlen, "Unsupported PS2A header size"); return 0; }
    index_count  = (int) rd32(data, 0x10);
    code_size    = (int) rd32(data, 0x14);
    vars_size    = (int) rd32(data, 0x18);
    strings_size = (int) rd32(data, 0x1C);
    entry        = (int) rd32(data, 0x20);
    if (index_count < 0 || code_size < 0 || vars_size < 0 || strings_size < 0) {
        fail(err, errlen, "PS2A header describes a script that cannot be right");
        return 0;
    }
    code_at = (long) HEADER + 4L * index_count;
    vars_at = code_at + code_size;
    strings_at = vars_at + vars_size;
    if (code_at > size || strings_at > size || strings_at + strings_size > size) {
        fail(err, errlen, "PS2A tables run past the end of the script");
        return 0;
    }
    out->data = data;
    out->size = size;
    out->index = (const uint32_t *) (const void *) (data + HEADER);
    out->index_count = index_count;
    out->code = data + code_at;
    out->code_size = code_size;
    out->vars_size = vars_size;
    out->strings = (const char *) data + strings_at;
    out->strings_size = strings_size;
    out->entry = (entry >= 0 && entry < code_size) ? entry : 0;
    return 1;
}

void cmvs_script_close(cmvs_script *s)
{
    if (!s) return;
    free(s->data);
    memset(s, 0, sizeof *s);
}

const char *cmvs_script_string(const cmvs_script *s, uint32_t offset)
{
    if (offset >= (uint32_t) s->strings_size) return NULL;
    return s->strings + offset;
}
