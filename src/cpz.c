#include "cpz.h"

#include <stdlib.h>
#include <string.h>

#include "md5.h"

#define MAX_ENTRY       (256 * 1024 * 1024)
#define MAX_INDEX       (32 * 1024 * 1024)
#define MAX_DIRECTORIES 4096
#define INIT_CHECKSUM   0x923A564Cu

/* ------------------------------------------------------------------ helpers */

static uint32_t rd32(const uint8_t *d, size_t at)
{
    return (uint32_t) d[at] | ((uint32_t) d[at + 1] << 8)
         | ((uint32_t) d[at + 2] << 16) | ((uint32_t) d[at + 3] << 24);
}

static void wr32(uint8_t *d, size_t at, uint32_t v)
{
    d[at] = (uint8_t) v;
    d[at + 1] = (uint8_t) (v >> 8);
    d[at + 2] = (uint8_t) (v >> 16);
    d[at + 3] = (uint8_t) (v >> 24);
}

static uint32_t rotl32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
static uint32_t rotr32(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }

static void fail(char *err, size_t errlen, const char *msg)
{
    if (err && errlen) { strncpy(err, msg, errlen - 1); err[errlen - 1] = 0; }
}

/* ------------------------------------------------------------------ decoder */

/* The substitution table CMVS derives from the archive keys. */
typedef struct {
    const cmvs_scheme *scheme;
    uint8_t table[0x100];
} decoder;

static void decoder_init(decoder *d, const cmvs_scheme *s, uint32_t key, uint32_t summand)
{
    int i;
    d->scheme = s;
    for (i = 0; i < 0x100; i++) d->table[i] = (uint8_t) i;
    for (i = 0; i < 0x100; i++) {
        unsigned a = (key >> 16) & 0xFF, b = key & 0xFF;
        uint8_t t = d->table[a];
        d->table[a] = d->table[b];
        d->table[b] = t;
        a = (key >> 8) & 0xFF;
        b = key >> 24;
        t = d->table[a];
        d->table[a] = d->table[b];
        d->table[b] = t;
        key = summand + s->decoder_factor * rotr32(key, 2);
    }
}

static void decoder_decode(decoder *d, uint8_t *data, size_t offset, size_t length, uint8_t key)
{
    size_t i;
    for (i = 0; i < length; i++) data[offset + i] = d->table[key ^ data[offset + i]];
}

static void decoder_decrypt_entry(decoder *d, uint8_t *data, int size,
                                  const uint32_t cmvs_md5_words[4], uint32_t seed)
{
    uint8_t key_bytes[0x40];
    uint32_t secret_key[0x10];
    uint32_t mangle, key;
    int i, words, k;

    for (i = 0; i < 0x10; i++) wr32(key_bytes, (size_t) i * 4, d->scheme->secret[i]);
    mangle = cmvs_md5_words[1] >> 2;
    for (i = 0; i < 0x40; i++) key_bytes[i] = (uint8_t) (mangle ^ d->table[key_bytes[i]]);
    for (i = 0; i < 0x10; i++) secret_key[i] = rd32(key_bytes, (size_t) i * 4) ^ seed;

    words = size / 4;
    key = d->scheme->entry_init_key;
    k = d->scheme->entry_key_pos & 0xF;
    for (i = 0; i < words; i++) {
        size_t at = (size_t) i * 4;
        uint32_t value = cmvs_md5_words[key & 3]
            ^ ((rd32(data, at) ^ secret_key[(key >> 6) & 0xF] ^ (secret_key[k] >> 1)) - seed);
        wr32(data, at, value);
        k = (k + 1) & 0xF;
        key += seed + value;
    }
    for (i = words * 4; i < size; i++) {
        data[i] = d->table[data[i] ^ d->scheme->entry_tail_key];
    }
}

/* -------------------------------------------------------------------- header */

typedef struct {
    int version;
    int dir_count;
    int dir_entries_size;
    int file_entries_size;
    uint32_t cmvs_md5[4];
    uint32_t master_key;
    int encrypted;
    uint32_t entry_key;
    int entry_name_offset;
    int index_offset;
    int index_size;
    uint8_t index_md5[16];
} cpz_header;

static uint32_t header_checksum(const uint8_t *d, int length, uint32_t crc)
{
    int i;
    for (i = 0; i < length / 4; i++) crc += rd32(d, (size_t) i * 4);
    for (i = length & ~3; i < length; i++) crc += d[i];
    return crc;
}

static int header_parse(cpz_header *h, const uint8_t *raw, long long file_length,
                        char *err, size_t errlen)
{
    if (raw[0] != 'C' || raw[1] != 'P' || raw[2] != 'Z') {
        fail(err, errlen, "Not a CPZ archive");
        return 0;
    }
    memset(h, 0, sizeof *h);
    h->version = raw[3] - '0';
    if (h->version != 5 && h->version != 6) {
        fail(err, errlen, "Only CPZ5 and CPZ6 archives are supported");
        return 0;
    }
    if (h->version < 6) {
        h->dir_count         = (int) (0xFE3A53D9u ^ rd32(raw, 4));
        h->dir_entries_size  = (int) (0x37F298E7u ^ rd32(raw, 8));
        h->file_entries_size = (int) (0x7A6F3A2Cu ^ rd32(raw, 0x0C));
        h->master_key        = 0xAE7D39BFu ^ rd32(raw, 0x30);
        h->encrypted         = 0 != (0xFB73A955u ^ rd32(raw, 0x34));
        h->entry_key         = 0;
        h->cmvs_md5[0] = 0x43DE7C19u ^ rd32(raw, 0x20);
        h->cmvs_md5[1] = 0xCC65F415u ^ rd32(raw, 0x24);
        h->cmvs_md5[2] = 0xD016A93Cu ^ rd32(raw, 0x28);
        h->cmvs_md5[3] = 0x97A3BA9Au ^ rd32(raw, 0x2C);
    } else {
        uint32_t seed = 0x37ACF832u ^ rd32(raw, 0x38);
        h->dir_count         = (int) (0xFE3A53DAu ^ rd32(raw, 4));
        h->dir_entries_size  = (int) (0x37F298E8u ^ rd32(raw, 8));
        h->file_entries_size = (int) (0x7A6F3A2Du ^ rd32(raw, 0x0C));
        h->master_key        = 0xAE7D39B7u ^ rd32(raw, 0x30);
        h->encrypted         = 0 != (0xFB73A956u ^ rd32(raw, 0x34));
        h->entry_key         = 0x7DA8F173u * rotr32(seed, 5) + 0x13712765u;
        h->cmvs_md5[0] = 0x43DE7C1Au ^ rd32(raw, 0x20);
        h->cmvs_md5[1] = 0xCC65F416u ^ rd32(raw, 0x24);
        h->cmvs_md5[2] = 0xD016A93Du ^ rd32(raw, 0x28);
        h->cmvs_md5[3] = 0x97A3BA9Bu ^ rd32(raw, 0x2C);
    }
    h->entry_name_offset = 0x18;
    h->index_offset = 0x40;
    if (h->dir_count <= 0 || h->dir_count > MAX_DIRECTORIES
        || h->dir_entries_size <= 0 || h->file_entries_size <= 0
        || h->dir_entries_size > MAX_INDEX || h->file_entries_size > MAX_INDEX) {
        fail(err, errlen, "CPZ header describes an index that cannot be right");
        return 0;
    }
    h->index_size = h->dir_entries_size + h->file_entries_size;
    if ((long long) h->index_offset + h->index_size > file_length) {
        fail(err, errlen, "CPZ index runs past the end of the archive");
        return 0;
    }
    if (rd32(raw, 0x3C) != header_checksum(raw, 0x3C, INIT_CHECKSUM)) {
        fail(err, errlen, "CPZ header checksum is wrong; the archive is damaged");
        return 0;
    }
    memcpy(h->index_md5, raw + 0x10, 16);
    return 1;
}

/* --------------------------------------------------------------------- index */

static void decrypt_index_stage1(uint8_t *data, int size, uint32_t key, const cmvs_scheme *s)
{
    uint32_t secret[24];
    int shift, words, i, n, si = 5;
    for (i = 0; i < 24; i++) secret[i] = s->secret[i] - key;
    shift = (int) ((((key >> 24) ^ (key >> 16) ^ (key >> 8) ^ key ^ 0xB) & 0xF) + 7);
    words = size / 4;
    for (i = 0; i < words; i++) {
        size_t at = (size_t) i * 4;
        wr32(data, at, rotr32((secret[si] ^ rd32(data, at)) + s->index_addend, shift) + 0x01010101u);
        si = (si + 1) % 24;
    }
    for (n = size & 3; n > 0; n--) {
        size_t at = (size_t) size - n;
        data[at] = (uint8_t) ((data[at] ^ (secret[si] >> (n * 4))) - s->index_subtrahend);
        si = (si + 1) % 24;
    }
}

static void decrypt_index_directory(uint8_t *data, int length, const uint32_t key[4])
{
    uint32_t seed = 0x76548AEFu;
    int words = length / 4, i = 0, j;
    for (; i < words; i++) {
        size_t at = (size_t) i * 4;
        wr32(data, at, rotl32((rd32(data, at) ^ key[i & 3]) - 0x4A91C262u, 3) - seed);
        seed += 0x10FB562Au;
    }
    for (j = length & 3; j > 0; j--) {
        size_t at = (size_t) length - j;
        data[at] = (uint8_t) ((data[at] ^ (key[i++ & 3] >> 6)) + 0x37);
    }
}

static void decrypt_index_entry(uint8_t *data, int offset, int length,
                                const uint32_t key[4], uint32_t seed)
{
    int words = length / 4, i = 0, j;
    for (; i < words; i++) {
        size_t at = (size_t) offset + (size_t) i * 4;
        wr32(data, at, rotl32((rd32(data, at) ^ key[i & 3]) - seed, 2) + 0x37A19E8Bu);
        seed -= 0x139FA9Bu;
    }
    for (j = length & 3; j > 0; j--) {
        size_t at = (size_t) offset + length - j;
        data[at] = (uint8_t) ((data[at] ^ (key[i++ & 3] >> 4)) + 5);
    }
}

/* cp932 name up to the first NUL; NULL if it does not look like one. */
static char *c_string(const uint8_t *data, int size, int at, int limit)
{
    int end, i;
    char *out;
    if (at < 0 || limit < 0 || at + limit > size) return NULL;
    end = at;
    while (end < at + limit && data[end] != 0) end++;
    for (i = at; i < end; i++) if (data[i] < 0x20) return NULL;
    out = malloc((size_t) (end - at) + 1);
    if (!out) return NULL;
    memcpy(out, data + at, (size_t) (end - at));
    out[end - at] = 0;
    return out;
}

static char *lowercase_slashes(const char *s)
{
    size_t n = strlen(s), i;
    char *out = malloc(n + 1);
    if (!out) return NULL;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\\') c = '/';
        else if (c >= 'A' && c <= 'Z') c = (char) (c - 'A' + 'a');
        out[i] = c;
    }
    out[n] = 0;
    return out;
}

/* ------------------------------------------------------------------- archive */

struct cpz_archive {
    FILE *fp;
    long long file_length;
    cpz_header header;
    const cmvs_scheme *scheme;
    decoder entry_decoder;
    cpz_entry *entries;
    int count;
};

static void free_entries(cpz_entry *e, int n)
{
    int i;
    for (i = 0; i < n; i++) { free(e[i].name); free(e[i].lookup); }
    free(e);
}

/* Returns 0 when this scheme clearly does not fit, so the next can be tried. */
static int read_index(cpz_header *cpz, const cmvs_scheme *s, uint8_t *index,
                      long long file_length, cpz_entry **out, int *out_count,
                      decoder *out_decoder)
{
    decoder dec;
    uint32_t key[4];
    long long base_offset;
    cpz_entry *found = NULL;
    int found_n = 0, found_cap = 0;
    int dir_offset = 0, i, j;

    cmvs_md5(s->md5_variant, cpz->cmvs_md5, cpz->cmvs_md5);
    decrypt_index_stage1(index, cpz->index_size, cpz->master_key ^ 0x3795B39Au, s);
    decoder_init(&dec, s, cpz->master_key, cpz->cmvs_md5[1]);
    decoder_decode(&dec, index, 0, (size_t) cpz->dir_entries_size, 0x3A);

    key[0] = cpz->cmvs_md5[0] ^ (cpz->master_key + 0x76A3BF29u);
    key[1] = cpz->cmvs_md5[1] ^ cpz->master_key;
    key[2] = cpz->cmvs_md5[2] ^ (cpz->master_key + 0x10000000u);
    key[3] = cpz->cmvs_md5[3] ^ cpz->master_key;
    decrypt_index_directory(index, cpz->dir_entries_size, key);

    decoder_init(&dec, s, cpz->master_key, cpz->cmvs_md5[2]);
    base_offset = (long long) cpz->index_offset + cpz->index_size;

    for (i = 0; i < cpz->dir_count; i++) {
        int dir_size, file_count, entries_offset, next_offset, size, cursor, end, is_root;
        uint32_t dir_key, entry_key[4];
        char *dir_name;

        if (dir_offset + 0x10 > cpz->dir_entries_size) goto reject;
        dir_size = (int) rd32(index, (size_t) dir_offset);
        if (dir_size <= 0x10 || dir_offset + dir_size > cpz->dir_entries_size) goto reject;
        file_count = (int) rd32(index, (size_t) dir_offset + 4);
        if (file_count < 0 || file_count >= 0x10000) goto reject;
        entries_offset = (int) rd32(index, (size_t) dir_offset + 8);
        dir_key = rd32(index, (size_t) dir_offset + 0x0C);
        dir_name = c_string(index, cpz->index_size, dir_offset + 0x10, dir_size - 0x10);
        if (!dir_name) goto reject;

        if (i + 1 == cpz->dir_count) {
            next_offset = cpz->file_entries_size;
        } else {
            if (dir_offset + dir_size + 12 > cpz->dir_entries_size) { free(dir_name); goto reject; }
            next_offset = (int) rd32(index, (size_t) dir_offset + dir_size + 8);
        }
        size = next_offset - entries_offset;
        if (entries_offset < 0 || size <= 0 || entries_offset + size > cpz->file_entries_size) {
            free(dir_name);
            goto reject;
        }

        cursor = cpz->dir_entries_size + entries_offset;
        end = cursor + size;
        decoder_decode(&dec, index, (size_t) cursor, (size_t) size, 0x7E);
        for (j = 0; j < 4; j++) entry_key[j] = cpz->cmvs_md5[j] ^ (dir_key + s->dir_key_addend[j]);
        decrypt_index_entry(index, cursor, size, entry_key, s->index_seed);

        is_root = strcmp(dir_name, "root") == 0;
        for (j = 0; j < file_count; j++) {
            int entry_size, entry_length;
            long long offset;
            char *name, *full;

            if (cursor + cpz->entry_name_offset > end) { free(dir_name); goto reject; }
            entry_size = (int) rd32(index, (size_t) cursor);
            if (entry_size <= cpz->entry_name_offset || cursor + entry_size > end) {
                free(dir_name);
                goto reject;
            }
            name = c_string(index, cpz->index_size, cursor + cpz->entry_name_offset,
                            end - cursor - cpz->entry_name_offset);
            if (!name || !*name) { free(name); free(dir_name); goto reject; }
            offset = (long long) ((uint64_t) rd32(index, (size_t) cursor + 4)
                     | ((uint64_t) rd32(index, (size_t) cursor + 8) << 32)) + base_offset;
            entry_length = (int) rd32(index, (size_t) cursor + 0x0C);
            if (entry_length < 0 || entry_length > MAX_ENTRY
                || offset < base_offset || offset + entry_length > file_length) {
                free(name);
                free(dir_name);
                goto reject;
            }
            if (is_root) {
                full = name;
            } else {
                size_t n = strlen(dir_name) + 1 + strlen(name) + 1;
                full = malloc(n);
                if (!full) { free(name); free(dir_name); goto reject; }
                snprintf(full, n, "%s/%s", dir_name, name);
                free(name);
            }
            if (found_n == found_cap) {
                int cap = found_cap ? found_cap * 2 : 64;
                cpz_entry *grown = realloc(found, (size_t) cap * sizeof *grown);
                if (!grown) { free(full); free(dir_name); goto reject; }
                found = grown;
                found_cap = cap;
            }
            found[found_n].name = full;
            found[found_n].lookup = lowercase_slashes(full);
            found[found_n].offset = offset;
            found[found_n].size = entry_length;
            found[found_n].key = rd32(index, (size_t) cursor + 0x14) + dir_key;
            if (!found[found_n].lookup) { free(dir_name); goto reject; }
            found_n++;
            cursor += entry_size;
        }
        free(dir_name);
        dir_offset += dir_size;
    }
    if (found_n == 0) goto reject;
    if (cpz->encrypted) decoder_init(&dec, s, cpz->cmvs_md5[3], cpz->master_key);
    *out = found;
    *out_count = found_n;
    *out_decoder = dec;
    return 1;

reject:
    free_entries(found, found_n);
    return 0;
}

cpz_archive *cpz_open(const char *path, char *err, size_t errlen)
{
    FILE *fp;
    long long length;
    uint8_t raw[0x40];
    cpz_header base;
    uint8_t *index = NULL, *work = NULL;
    uint8_t digest[16];
    int i;

    fp = fopen(path, "rb");
    if (!fp) { fail(err, errlen, "Cannot open the archive"); return NULL; }
    if (fseek(fp, 0, SEEK_END) != 0) { fail(err, errlen, "Cannot size the archive"); goto bad; }
    length = ftell(fp);
    if (length < 0x40) { fail(err, errlen, "File is too small to be a CPZ archive"); goto bad; }
    rewind(fp);
    if (fread(raw, 1, sizeof raw, fp) != sizeof raw) {
        fail(err, errlen, "Cannot read the CPZ header");
        goto bad;
    }
    if (!header_parse(&base, raw, length, err, errlen)) goto bad;

    index = malloc((size_t) base.index_size);
    if (!index) { fail(err, errlen, "Out of memory reading the CPZ index"); goto bad; }
    if (fseek(fp, base.index_offset, SEEK_SET) != 0
        || fread(index, 1, (size_t) base.index_size, fp) != (size_t) base.index_size) {
        fail(err, errlen, "Cannot read the CPZ index");
        goto bad;
    }
    cmvs_plain_md5(index, (size_t) base.index_size, digest);
    if (memcmp(digest, base.index_md5, 16) != 0) {
        fail(err, errlen, "CPZ index does not match its own MD5; the archive is damaged");
        goto bad;
    }

    work = malloc((size_t) base.index_size);
    if (!work) { fail(err, errlen, "Out of memory reading the CPZ index"); goto bad; }
    for (i = 0; i < cmvs_scheme_count; i++) {
        cpz_header attempt = base;
        cpz_entry *entries = NULL;
        int count = 0;
        decoder dec;
        memcpy(work, index, (size_t) base.index_size);
        if (read_index(&attempt, &cmvs_schemes[i], work, length, &entries, &count, &dec)) {
            cpz_archive *a = calloc(1, sizeof *a);
            if (!a) { free_entries(entries, count); fail(err, errlen, "Out of memory"); goto bad; }
            a->fp = fp;
            a->file_length = length;
            a->header = attempt;
            a->scheme = &cmvs_schemes[i];
            a->entry_decoder = dec;
            a->entries = entries;
            a->count = count;
            free(index);
            free(work);
            return a;
        }
    }
    fail(err, errlen, "No known CMVS encryption scheme opens this archive; "
                      "the game's scheme has not been added yet");
bad:
    free(index);
    free(work);
    fclose(fp);
    return NULL;
}

void cpz_close(cpz_archive *a)
{
    if (!a) return;
    free_entries(a->entries, a->count);
    fclose(a->fp);
    free(a);
}

const char *cpz_scheme_name(const cpz_archive *a) { return a->scheme->name; }
int cpz_count(const cpz_archive *a) { return a->count; }

const cpz_entry *cpz_at(const cpz_archive *a, int index)
{
    return (index < 0 || index >= a->count) ? NULL : &a->entries[index];
}

const cpz_entry *cpz_find(const cpz_archive *a, const char *name)
{
    char *want = lowercase_slashes(name);
    int i;
    const cpz_entry *hit = NULL;
    if (!want) return NULL;
    for (i = 0; i < a->count; i++) {
        if (strcmp(a->entries[i].lookup, want) == 0) { hit = &a->entries[i]; break; }
    }
    free(want);
    return hit;
}

void cmvs_decrypt_pb3(uint8_t *data, int size)
{
    int key1 = data[size - 3], key2 = data[size - 2];
    int src = size - 0x2F, i;
    for (i = 8; i < 0x34; i += 2) {
        data[i] = (uint8_t) ((data[i] ^ key1) - data[src++]);
        data[i + 1] = (uint8_t) ((data[i + 1] ^ key2) - data[src++]);
    }
}

uint8_t *cmvs_unpack_ps2(const uint8_t *src, int size, int *size_out, char *err, size_t errlen)
{
    uint8_t *body, *out, frame[0x800];
    uint32_t seed;
    int shift, key, unpacked, i;
    int frame_pos = 0x7DF, in = 0x30, dst = 0x30, control = 1;

    if (size <= 0x30) { fail(err, errlen, "PS2A container is truncated"); return NULL; }
    body = malloc((size_t) size);
    if (!body) { fail(err, errlen, "Out of memory unpacking a PS2A container"); return NULL; }
    memcpy(body, src, (size_t) size);

    seed = rd32(body, 12);
    shift = (int) ((seed >> 20) % 5) + 1;
    key = (int) (((seed >> 24) + (seed >> 3)) & 0xFF);
    for (i = 0x30; i < size; i++) {
        int value = (key ^ (body[i] - 0x7C)) & 0xFF;
        body[i] = (uint8_t) (((value >> shift) | (value << (8 - shift))) & 0xFF);
    }
    unpacked = (int) rd32(body, 0x28);
    if (unpacked < 0 || unpacked > MAX_ENTRY) {
        fail(err, errlen, "PS2A unpacked size is out of range");
        free(body);
        return NULL;
    }
    out = malloc((size_t) 0x30 + unpacked);
    if (!out) { fail(err, errlen, "Out of memory unpacking a PS2A container"); free(body); return NULL; }
    memcpy(out, body, 0x30);
    memset(frame, 0, sizeof frame);

    while (dst < 0x30 + unpacked && in < size) {
        if (control == 1) control = body[in++] | 0x100;
        if (control & 1) {
            uint8_t value = body[in++];
            out[dst++] = value;
            frame[frame_pos++ & 0x7FF] = value;
        } else {
            int lo, hi, offset, count;
            if (in + 1 >= size) break;
            lo = body[in++];
            hi = body[in++];
            offset = lo | ((hi & 0xE0) << 3);
            count = (hi & 0x1F) + 2;
            for (i = 0; i < count && dst < 0x30 + unpacked; i++) {
                uint8_t value = frame[(offset + i) & 0x7FF];
                out[dst++] = value;
                frame[frame_pos++ & 0x7FF] = value;
            }
        }
        control >>= 1;
    }
    free(body);
    if (dst != 0x30 + unpacked) {
        fail(err, errlen, "PS2A stream ended before it was fully expanded");
        free(out);
        return NULL;
    }
    *size_out = 0x30 + unpacked;
    return out;
}

uint8_t *cpz_read(cpz_archive *a, const cpz_entry *e, int *size_out, char *err, size_t errlen)
{
    uint8_t *data = malloc((size_t) e->size ? (size_t) e->size : 1);
    if (!data) { fail(err, errlen, "Out of memory reading an archive entry"); return NULL; }
    if (fseek(a->fp, (long) e->offset, SEEK_SET) != 0
        || fread(data, 1, (size_t) e->size, a->fp) != (size_t) e->size) {
        fail(err, errlen, "Cannot read an archive entry");
        free(data);
        return NULL;
    }
    if (a->header.encrypted) {
        uint32_t key = (a->header.master_key ^ e->key) + (uint32_t) a->header.dir_count;
        key -= a->scheme->entry_sub_key;
        key ^= a->header.entry_key;
        decoder_decrypt_entry(&a->entry_decoder, data, e->size, a->header.cmvs_md5, key);
    }
    if (e->size > 0x30 && !memcmp(data, "PS2A", 4)) {
        int n = 0;
        uint8_t *out = cmvs_unpack_ps2(data, e->size, &n, err, errlen);
        free(data);
        if (!out) return NULL;
        *size_out = n;
        return out;
    }
    if (e->size > 0x40 && !memcmp(data, "PB3B", 4)) cmvs_decrypt_pb3(data, e->size);
    *size_out = e->size;
    return data;
}
