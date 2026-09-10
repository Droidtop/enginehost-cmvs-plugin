#include "save.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lzss.h"

static void fail(char *err, size_t errlen, const char *msg)
{
    if (err && errlen) snprintf(err, errlen, "%s", msg);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16)
         | ((uint32_t) p[3] << 24);
}
static unsigned rd16(const uint8_t *p) { return (unsigned) p[0] | ((unsigned) p[1] << 8); }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8);
    p[2] = (uint8_t) (v >> 16); p[3] = (uint8_t) (v >> 24);
}
static void wr16(uint8_t *p, unsigned v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); }

/*
 * The tag table. Every tag the writer at 0x0045bea0 emits, in the shape the
 * reader at 0x0045cb30 expects. The five indexed tags at the end are not in
 * ChronoClock's prologue saves but are in the writer, so they are here: a file
 * from a later scene must not stop this reader.
 */
cmvs_rec_shape cmvs_record_shape(unsigned tag)
{
    switch (tag) {
    case 0x100: case 0x101: case 0x102: case 0x103: case 0x104: case 0x105:
    case 0x106: case 0x107: case 0x108: case 0x109: case 0x10a: case 0x10b:
    case 0x112: case 0x113: case 0x114: case 0x115:
    case 0x200: case 0x201: case 0x202: case 0x203:
    case 0x240: case 0x241: case 0x248:
        return CMVS_REC_SHORT;
    case 0x242: case 0x243:
    case 0x280: case 0x281: case 0x282: case 0x283:
    case 0x310: case 0x318: case 0x540: case 0x6e0:
    case 0x880: case 0x881: case 0x882: case 0x883: case 0x900:
        return CMVS_REC_LONG;
    case 0x380: case 0x400: case 0x700: case 0x6a0: case 0x800:
    case 0x8c0: case 0x8c8: case 0x8e8:
        return CMVS_REC_INDEXED;
    case 0x304:
        return CMVS_REC_MUSIC;
    default:
        return CMVS_REC_UNKNOWN;
    }
}

void cmvs_save_init(cmvs_save *s)
{
    memset(s, 0, sizeof *s);
}

void cmvs_save_free(cmvs_save *s)
{
    int i;
    if (!s) return;
    for (i = 0; i < s->records; i++) free(s->rec[i].data);
    free(s->rec);
    free(s->thumb);
    free(s->script);
    memset(s, 0, sizeof *s);
}

cmvs_record *cmvs_save_find(const cmvs_save *s, unsigned tag, int index)
{
    int i;
    for (i = 0; i < s->records; i++)
        if (s->rec[i].tag == tag && s->rec[i].index == index)
            return &s->rec[i];
    return NULL;
}

cmvs_record *cmvs_save_set(cmvs_save *s, unsigned tag, int index,
                           const void *data, int len)
{
    cmvs_record *r = cmvs_save_find(s, tag, index);
    uint8_t *copy;

    if (len < 0) return NULL;
    copy = malloc((size_t) len + 1);      /* +1 so a zero-length record has a pointer */
    if (!copy) return NULL;
    if (len) memcpy(copy, data, (size_t) len);

    if (!r) {
        if (s->records == s->capacity) {
            int want = s->capacity ? s->capacity * 2 : 64;
            cmvs_record *grown = realloc(s->rec, sizeof *grown * (size_t) want);
            if (!grown) { free(copy); return NULL; }
            s->rec = grown;
            s->capacity = want;
        }
        r = &s->rec[s->records++];
        memset(r, 0, sizeof *r);
        r->tag = tag;
        r->index = index;
    } else {
        free(r->data);
    }
    r->data = copy;
    r->len = len;
    return r;
}

int cmvs_save_clone(cmvs_save *dst, const cmvs_save *src)
{
    int i;
    cmvs_save_init(dst);
    memcpy(dst->header, src->header, CMVS_SAVE_HEADER);
    for (i = 0; i < src->records; i++)
        if (!cmvs_save_set(dst, src->rec[i].tag, src->rec[i].index,
                           src->rec[i].data, src->rec[i].len)) {
            cmvs_save_free(dst);
            return 0;
        }
    if (src->thumb && src->thumb_size) {
        dst->thumb = malloc((size_t) src->thumb_size);
        if (!dst->thumb) { cmvs_save_free(dst); return 0; }
        memcpy(dst->thumb, src->thumb, (size_t) src->thumb_size);
        dst->thumb_size = src->thumb_size;
    }
    if (src->script && src->script_size) {
        dst->script = malloc((size_t) src->script_size);
        if (!dst->script) { cmvs_save_free(dst); return 0; }
        memcpy(dst->script, src->script, (size_t) src->script_size);
        dst->script_size = src->script_size;
    }
    return 1;
}

/* --------------------------------------------------------------- the cipher */

/* 0x0046f970. Only stream A is ciphered, with the key at 0x54a434. */
static const char csv2_key[] = "DAME_SAVE_IMAGE";
#define CSV2_KEYLEN 15

static void csv2_encipher(uint8_t *buf, int size)
{
    int i, k = 0;
    for (i = 0; i < size; i++) {
        buf[i] = (uint8_t) (((buf[i] - 0x80) & 0xFF) ^ (uint8_t) csv2_key[k]);
        k = (k + 1) % CSV2_KEYLEN;
    }
}

static void csv2_decipher(uint8_t *buf, int size)
{
    int i, k = 0;
    for (i = 0; i < size; i++) {
        buf[i] = (uint8_t) (((buf[i] ^ (uint8_t) csv2_key[k]) + 0x80) & 0xFF);
        k = (k + 1) % CSV2_KEYLEN;
    }
}

static unsigned sum16(const uint8_t *buf, int size)
{
    unsigned total = 0;
    int i;
    for (i = 0; i < size; i++) total += buf[i];
    return total & 0xFFFF;
}

/* ------------------------------------------------------------ record list */

static int parse_records(cmvs_save *s, const uint8_t *a, int size,
                         char *err, size_t errlen)
{
    int p = 0;
    while (p < size) {
        unsigned tag;
        int index = 0, len, head;
        if (p + 4 > size) { fail(err, errlen, "record list ends inside a header"); return 0; }
        tag = rd16(a + p);
        switch (cmvs_record_shape(tag)) {
        case CMVS_REC_SHORT:
            len = (int) rd16(a + p + 2);
            head = 4;
            break;
        case CMVS_REC_LONG:
            if (p + 6 > size) { fail(err, errlen, "record list ends inside a header"); return 0; }
            len = (int) rd32(a + p + 2);
            head = 6;
            break;
        case CMVS_REC_INDEXED:
            if (p + 8 > size) { fail(err, errlen, "record list ends inside a header"); return 0; }
            index = (int) rd16(a + p + 2);
            len = (int) rd32(a + p + 4);
            head = 8;
            break;
        case CMVS_REC_MUSIC: {
            /* Its payload is its own two counted names, so the record keeps
             * everything after the tag and the lengths stay where they are. */
            int l1, l2;
            l1 = (int) rd16(a + p + 2);
            if (p + 4 + l1 + 2 > size) { fail(err, errlen, "music record runs off the end"); return 0; }
            l2 = (int) rd16(a + p + 4 + l1);
            len = 2 + l1 + 2 + l2;
            head = 2;
            break;
        }
        default:
            if (err && errlen)
                snprintf(err, errlen, "unknown record tag 0x%03x at %d - "
                         "a save this reader cannot follow", tag, p);
            return 0;
        }
        if (len < 0 || p + head + len > size) {
            fail(err, errlen, "record runs off the end of the list");
            return 0;
        }
        if (!cmvs_save_set(s, tag, index, a + p + head, len)) {
            fail(err, errlen, "out of memory reading the record list");
            return 0;
        }
        p += head + len;
    }
    return 1;
}

static int record_bytes(const cmvs_record *r)
{
    switch (cmvs_record_shape(r->tag)) {
    case CMVS_REC_SHORT: return 4 + r->len;
    case CMVS_REC_LONG:  return 6 + r->len;
    case CMVS_REC_INDEXED: return 8 + r->len;
    case CMVS_REC_MUSIC: return 2 + r->len;   /* the two lengths are in the payload */
    default: return -1;
    }
}

static uint8_t *build_records(const cmvs_save *s, int *size_out, char *err, size_t errlen)
{
    int total = 0, i, p = 0;
    uint8_t *a;

    for (i = 0; i < s->records; i++) {
        int n = record_bytes(&s->rec[i]);
        if (n < 0) {
            if (err && errlen)
                snprintf(err, errlen, "record tag 0x%03x has no shape - refusing to "
                         "write a file the original cannot read", s->rec[i].tag);
            return NULL;
        }
        total += n;
    }
    a = malloc((size_t) total + 1);
    if (!a) { fail(err, errlen, "out of memory building the record list"); return NULL; }
    for (i = 0; i < s->records; i++) {
        const cmvs_record *r = &s->rec[i];
        wr16(a + p, r->tag);
        switch (cmvs_record_shape(r->tag)) {
        case CMVS_REC_SHORT:
            wr16(a + p + 2, (unsigned) r->len);
            p += 4;
            break;
        case CMVS_REC_LONG:
            wr32(a + p + 2, (uint32_t) r->len);
            p += 6;
            break;
        case CMVS_REC_INDEXED:
            wr16(a + p + 2, (unsigned) r->index);
            wr32(a + p + 4, (uint32_t) r->len);
            p += 8;
            break;
        default:            /* music: the payload carries both lengths already */
            p += 2;
            break;
        }
        if (r->len) memcpy(a + p, r->data, (size_t) r->len);
        p += r->len;
    }
    *size_out = total;
    return a;
}

/* ------------------------------------------------------------------- CSV2 */

int cmvs_save_read(const uint8_t *file, int size, cmvs_save *out,
                   char *err, size_t errlen)
{
    uint32_t ca, cb, cc, ua, ub, uc, total;
    uint8_t *a = NULL, *plain = NULL;
    int ok = 0;

    cmvs_save_init(out);
    if (size < CMVS_SAVE_HEADER + 2) { fail(err, errlen, "too short for a CSV2 header"); return 0; }
    if (memcmp(file, "CSV2", 4) != 0) { fail(err, errlen, "not a CSV2 save"); return 0; }
    if (rd32(file + 4) != CMVS_SAVE_HEADER) { fail(err, errlen, "CSV2 header is not 0x258 bytes"); return 0; }

    memcpy(out->header, file, CMVS_SAVE_HEADER);
    total = rd32(file + CMVS_SAVE_TOTAL);
    ca = rd32(file + 0x230); cb = rd32(file + 0x234); cc = rd32(file + 0x238);
    ua = rd32(file + 0x244); ub = rd32(file + 0x248); uc = rd32(file + 0x24c);

    if ((uint32_t) size != total + 2) { fail(err, errlen, "file size does not match header 0x21c"); goto done; }
    if (CMVS_SAVE_HEADER + ca + cb + cc != total) { fail(err, errlen, "stream sizes do not add up to header 0x21c"); goto done; }

    if (rd16(file + size - 2) != sum16(file + CMVS_SAVE_HEADER, (int) ca)) {
        fail(err, errlen, "checksum does not match the ciphered state stream");
        goto done;
    }

    /* Stream A: decipher a copy, then expand. */
    a = malloc((size_t) ca + 1);
    plain = malloc((size_t) ua + 1);
    if (!a || !plain) { fail(err, errlen, "out of memory"); goto done; }
    memcpy(a, file + CMVS_SAVE_HEADER, (size_t) ca);
    csv2_decipher(a, (int) ca);
    if (cmvs_lzss_expand(&cmvs_lzss_csv2, a, (int) ca, plain, (int) ua) != (int) ua) {
        fail(err, errlen, "the state stream did not expand to its declared size");
        goto done;
    }
    if (!parse_records(out, plain, (int) ua, err, errlen)) goto done;

    if (rd16(file + CMVS_SAVE_HAS_THUMB) && cb) {
        out->thumb = malloc((size_t) ub + 1);
        if (!out->thumb) { fail(err, errlen, "out of memory"); goto done; }
        if (cmvs_lzss_expand(&cmvs_lzss_csv2, file + CMVS_SAVE_HEADER + ca, (int) cb,
                             out->thumb, (int) ub) != (int) ub) {
            fail(err, errlen, "the thumbnail did not expand to its declared size");
            goto done;
        }
        out->thumb_size = (int) ub;
    }
    if (cc) {
        out->script = malloc((size_t) uc + 1);
        if (!out->script) { fail(err, errlen, "out of memory"); goto done; }
        if (cmvs_lzss_expand(&cmvs_lzss_csv2, file + CMVS_SAVE_HEADER + ca + cb, (int) cc,
                             out->script, (int) uc) != (int) uc) {
            fail(err, errlen, "the script image did not expand to its declared size");
            goto done;
        }
        out->script_size = (int) uc;
    }
    ok = 1;
done:
    free(a);
    free(plain);
    if (!ok) cmvs_save_free(out);
    return ok;
}

uint8_t *cmvs_save_write(const cmvs_save *s, int *size_out, char *err, size_t errlen)
{
    uint8_t *plain = NULL, *ca = NULL, *cb = NULL, *cc = NULL, *file = NULL;
    int ua = 0, ca_size = 0, cb_size = 0, cc_size = 0, total, p;

    plain = build_records(s, &ua, err, errlen);
    if (!plain) return NULL;
    ca = cmvs_lzss_compress(&cmvs_lzss_csv2, plain, ua, &ca_size);
    if (!ca) { fail(err, errlen, "out of memory compressing the state"); goto oops; }
    csv2_encipher(ca, ca_size);

    if (s->thumb && s->thumb_size) {
        cb = cmvs_lzss_compress(&cmvs_lzss_csv2, s->thumb, s->thumb_size, &cb_size);
        if (!cb) { fail(err, errlen, "out of memory compressing the thumbnail"); goto oops; }
    }
    if (s->script && s->script_size) {
        cc = cmvs_lzss_compress(&cmvs_lzss_csv2, s->script, s->script_size, &cc_size);
        if (!cc) { fail(err, errlen, "out of memory compressing the script image"); goto oops; }
    }

    total = CMVS_SAVE_HEADER + ca_size + cb_size + cc_size;
    file = malloc((size_t) total + 2);
    if (!file) { fail(err, errlen, "out of memory"); goto oops; }
    memcpy(file, s->header, CMVS_SAVE_HEADER);
    memcpy(file, "CSV2", 4);
    wr32(file + 4, CMVS_SAVE_HEADER);
    wr32(file + CMVS_SAVE_TOTAL, (uint32_t) total);
    wr16(file + 0x220, 4);
    wr16(file + 0x222, 1);
    wr16(file + CMVS_SAVE_HAS_THUMB, cb_size ? 1 : 0);
    wr32(file + 0x230, (uint32_t) ca_size);
    wr32(file + 0x234, (uint32_t) cb_size);
    wr32(file + 0x238, (uint32_t) cc_size);
    wr32(file + 0x244, (uint32_t) ua);
    wr32(file + 0x248, (uint32_t) (cb_size ? s->thumb_size : 0));
    wr32(file + 0x24c, (uint32_t) (cc_size ? s->script_size : 0));

    p = CMVS_SAVE_HEADER;
    memcpy(file + p, ca, (size_t) ca_size); p += ca_size;
    if (cb_size) { memcpy(file + p, cb, (size_t) cb_size); p += cb_size; }
    if (cc_size) { memcpy(file + p, cc, (size_t) cc_size); p += cc_size; }
    wr16(file + p, sum16(file + CMVS_SAVE_HEADER, ca_size));

    free(plain); free(ca); free(cb); free(cc);
    if (size_out) *size_out = total + 2;
    return file;
oops:
    free(plain); free(ca); free(cb); free(cc); free(file);
    return NULL;
}

/*
 * 0x0046f844 re-stamps the digits of the caption with the time the file is
 * actually written, leaving the separators and the title after them alone.
 * Those offsets are the digits of "YYYY-MM-DD HH:MM:SS ".
 */
void cmvs_save_stamp(cmvs_save *s, int year, int mon, int day,
                     int hour, int min, int sec)
{
    char *c = (char *) s->header + CMVS_SAVE_CAPTION;
    c[0] = (char) ('0' + year / 1000 % 10);
    c[1] = (char) ('0' + year / 100 % 10);
    c[2] = (char) ('0' + year / 10 % 10);
    c[3] = (char) ('0' + year % 10);
    c[5] = (char) ('0' + mon / 10 % 10);   c[6] = (char) ('0' + mon % 10);
    c[8] = (char) ('0' + day / 10 % 10);   c[9] = (char) ('0' + day % 10);
    c[11] = (char) ('0' + hour / 10 % 10); c[12] = (char) ('0' + hour % 10);
    c[14] = (char) ('0' + min / 10 % 10);  c[15] = (char) ('0' + min % 10);
    c[17] = (char) ('0' + sec / 10 % 10);  c[18] = (char) ('0' + sec % 10);
}

/* ------------------------------------------------------------------- CSS1 */

/* 0x0041525f, key at 0x54901c. */
static const char css1_key[] = "YUKO_KAWAI_PURPLE_SYSTEM_SAVE_CHECK";
#define CSS1_KEYLEN 35

static uint8_t rol8(uint8_t v, int by) { return (uint8_t) ((v << by) | (v >> (8 - by))); }
static uint8_t ror8(uint8_t v, int by) { return (uint8_t) ((v >> by) | (v << (8 - by))); }

static int css1_rot(const uint8_t *header)
{
    unsigned a = rd16(header + 0x4e), c = rd16(header + 0x52);
    int rot = (int) ((c >> ((a >> 1) & 7)) & 3) + 2;
    if (rot > 6) rot = 6;
    return rot;
}

static void css1_crypt(const uint8_t *header, uint8_t *buf, int size, int encrypt)
{
    uint8_t a = (uint8_t) rd16(header + 0x4e), b = (uint8_t) rd16(header + 0x50);
    int rot = css1_rot(header), i, k = 0;

    for (i = 0; i < size; i++) {
        uint8_t mixed = (uint8_t) ((uint8_t) css1_key[k] + b);
        if (encrypt) buf[i] = rol8((uint8_t) ((mixed ^ (uint8_t) (buf[i] + 0x40)) + a), rot);
        else         buf[i] = (uint8_t) (((uint8_t) (ror8(buf[i], rot) - a) ^ mixed) - 0x40);
        k = (k + 1) % CSS1_KEYLEN;
    }
}

void cmvs_system_init(cmvs_system *s) { memset(s, 0, sizeof *s); }

void cmvs_system_free(cmvs_system *s)
{
    if (!s) return;
    free(s->payload);
    memset(s, 0, sizeof *s);
}

int cmvs_system_read(const uint8_t *file, int size, cmvs_system *out,
                     char *err, size_t errlen)
{
    uint32_t packed, expanded, total;
    uint8_t *work = NULL;
    int ok = 0;

    cmvs_system_init(out);
    if (size < CMVS_SYSTEM_HEADER + 2) { fail(err, errlen, "too short for a CSS1 header"); return 0; }
    if (memcmp(file, "CSS1", 4) != 0) { fail(err, errlen, "not a CSS1 system file"); return 0; }

    memcpy(out->header, file, CMVS_SYSTEM_HEADER);
    expanded = rd32(file + 0x5c);
    packed = rd32(file + 0x60);
    total = rd32(file + 0x64);
    if (total != packed + CMVS_SYSTEM_HEADER) { fail(err, errlen, "header 0x64 is not 0x168 + the packed size"); return 0; }
    if ((uint32_t) size != total + 2) { fail(err, errlen, "file size does not match header 0x64"); return 0; }
    if (rd16(file + size - 2) != sum16(file + CMVS_SYSTEM_HEADER, (int) packed)) {
        fail(err, errlen, "checksum does not match the ciphered payload");
        return 0;
    }

    work = malloc((size_t) packed + 1);
    out->payload = malloc((size_t) expanded + 1);
    if (!work || !out->payload) { fail(err, errlen, "out of memory"); goto done; }
    memcpy(work, file + CMVS_SYSTEM_HEADER, (size_t) packed);
    css1_crypt(out->header, work, (int) packed, 0);
    if (cmvs_lzss_expand(&cmvs_lzss_css1, work, (int) packed,
                         out->payload, (int) expanded) != (int) expanded) {
        fail(err, errlen, "the payload did not expand to its declared size");
        goto done;
    }
    out->size = (int) expanded;
    ok = 1;
done:
    free(work);
    if (!ok) cmvs_system_free(out);
    return ok;
}

void cmvs_system_params(cmvs_system *s, unsigned seed)
{
    wr16(s->header + 0x4e, seed & 0xFF);
    wr16(s->header + 0x50, ((seed >> 8) & 0x7F) + 0x80);
    wr16(s->header + 0x52, (seed >> 3) & 0xFFFF);
}

uint8_t *cmvs_system_write(const cmvs_system *s, int *size_out,
                           char *err, size_t errlen)
{
    uint8_t *packed, *file;
    int packed_size = 0;

    packed = cmvs_lzss_compress(&cmvs_lzss_css1, s->payload, s->size, &packed_size);
    if (!packed) { fail(err, errlen, "out of memory compressing the system payload"); return NULL; }

    file = malloc((size_t) CMVS_SYSTEM_HEADER + (size_t) packed_size + 2);
    if (!file) { free(packed); fail(err, errlen, "out of memory"); return NULL; }
    memcpy(file, s->header, CMVS_SYSTEM_HEADER);
    memcpy(file, "CSS1", 4);
    wr32(file + 4, 0x68);
    wr16(file + 0x4c, 1);

    /*
     * The three cipher parameters ride along from the header the caller holds:
     * the original draws them from rand() on every write and reads them back
     * out of the file, so any values do, and taking them rather than drawing
     * them here is what makes rewriting a file reproduce it exactly.
     */
    wr32(file + 0x5c, (uint32_t) s->size);
    wr32(file + 0x60, (uint32_t) packed_size);
    wr32(file + 0x64, (uint32_t) (CMVS_SYSTEM_HEADER + packed_size));

    css1_crypt(file, packed, packed_size, 1);
    memcpy(file + CMVS_SYSTEM_HEADER, packed, (size_t) packed_size);
    wr16(file + CMVS_SYSTEM_HEADER + packed_size,
         sum16(file + CMVS_SYSTEM_HEADER, packed_size));
    free(packed);
    if (size_out) *size_out = CMVS_SYSTEM_HEADER + packed_size + 2;
    return file;
}
