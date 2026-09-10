/*
 * The two save containers CMVS writes, read and written the way the original
 * writes them.
 *
 * The user's rule for this engine: a save of ours is a PC save. Same file
 * names, same bytes, in the folder the host gives us - so a slot written on the
 * console loads in the Windows game and a slot written there loads here. That
 * rules out a format of our own, and it is why everything below is a
 * transcription rather than a design.
 *
 *   save%03d.dat   one slot, magic CSV2, written by 0x0046f490
 *   save999.dat    the quick slot, the same format with slot 0x3e7
 *   system.dat     the persistent globals, magic CSS1, written by 0x00414f50
 *   system.bak     a copy of the previous system.dat, taken before each rewrite
 *
 * The full derivation, with addresses and the reference files it was checked
 * against, is in coordination agents/cmvs/SAVE-FORMAT.md.
 *
 * A CSV2 file is a 600-byte header and three LZSS streams: the state record
 * list (also ciphered), the thumbnail, and a copy of the script the resume
 * point is in. The record list is a flat list of tagged records whose HEADER
 * SHAPE DEPENDS ON THE TAG - the original's reader skips a tag it does not know
 * without consuming its payload, so an invented tag desynchronises it and a
 * writer must never emit one. That is also why reading stops on an unknown tag
 * here rather than trying to resynchronise: there is nothing to resynchronise
 * to.
 */
#ifndef CMVS_SAVE_H
#define CMVS_SAVE_H

#include <stddef.h>
#include <stdint.h>

#define CMVS_SAVE_HEADER   0x258
#define CMVS_SYSTEM_HEADER 0x168

/* The header fields a caller reads or writes by name. The rest travels
 * verbatim in cmvs_save.header. */
#define CMVS_SAVE_CAPTION      0x010   /* char[0x100]: "%Y-%m-%d %H:%M:%S <title>" */
#define CMVS_SAVE_CAPTION2     0x110   /* char[0x100]: a second caption, empty in ChronoClock */
#define CMVS_SAVE_TOTAL        0x21C
#define CMVS_SAVE_HAS_THUMB    0x224

typedef enum {
    CMVS_REC_SHORT = 0,   /* u16 tag; u16 len; data */
    CMVS_REC_LONG,        /* u16 tag; u32 len; data */
    CMVS_REC_INDEXED,     /* u16 tag; u16 index; u32 len; data */
    CMVS_REC_MUSIC,       /* u16 tag; u16 l1; name[l1]; u16 l2; name[l2] */
    CMVS_REC_UNKNOWN
} cmvs_rec_shape;

cmvs_rec_shape cmvs_record_shape(unsigned tag);

typedef struct {
    unsigned tag;
    int index;          /* CMVS_REC_INDEXED only */
    uint8_t *data;      /* the payload, owned */
    int len;
} cmvs_record;

typedef struct {
    uint8_t header[CMVS_SAVE_HEADER];

    cmvs_record *rec;
    int records;
    int capacity;

    uint8_t *thumb;     /* stream B, a 24-bit BMP; NULL when the header says none */
    int thumb_size;
    uint8_t *script;    /* stream C, the PS2A container the resume point is in */
    int script_size;
} cmvs_save;

void cmvs_save_init(cmvs_save *s);
void cmvs_save_free(cmvs_save *s);

/* Reads one slot file. Returns 1, or 0 with err saying which check failed. */
int cmvs_save_read(const uint8_t *file, int size, cmvs_save *out,
                   char *err, size_t errlen);

/*
 * Writes one slot file into a malloc'd buffer the caller frees. Deterministic:
 * the same save writes the same bytes, which is what lets a round-trip be
 * checked by comparison rather than by inspection.
 */
uint8_t *cmvs_save_write(const cmvs_save *s, int *size_out, char *err, size_t errlen);

/* A deep copy, so a state can keep the record list it came from without
 * holding on to the buffer the file was read into. */
int cmvs_save_clone(cmvs_save *dst, const cmvs_save *src);

/* The record with this tag (and index, for an indexed one), or NULL. */
cmvs_record *cmvs_save_find(const cmvs_save *s, unsigned tag, int index);

/*
 * Puts a record in place, replacing one with the same tag and index or adding
 * it at the end. Returns the record, or NULL if it could not be stored.
 */
cmvs_record *cmvs_save_set(cmvs_save *s, unsigned tag, int index,
                           const void *data, int len);

/* Stamps the timestamp digits of the caption in place the way the file writer
 * does at 0x0046f844: the digits only, never the separators or the title. */
void cmvs_save_stamp(cmvs_save *s, int year, int mon, int day,
                     int hour, int min, int sec);

/* ------------------------------------------------------------- system.dat */

typedef struct {
    uint8_t header[CMVS_SYSTEM_HEADER];
    uint8_t *payload;
    int size;
} cmvs_system;

void cmvs_system_init(cmvs_system *s);
void cmvs_system_free(cmvs_system *s);
int cmvs_system_read(const uint8_t *file, int size, cmvs_system *out,
                     char *err, size_t errlen);
/*
 * The three cipher parameters at header 0x4e/0x50/0x52 are chosen at random by
 * the original on every write and stored in the file, so two system.dat files
 * written from the same settings are never byte-equal. The writer here takes
 * them from the header it is given rather than drawing its own, which is what
 * lets a file be rewritten byte for byte; cmvs_system_params sets them for a
 * file being written fresh.
 */
void cmvs_system_params(cmvs_system *s, unsigned seed);
uint8_t *cmvs_system_write(const cmvs_system *s, int *size_out,
                           char *err, size_t errlen);

/* Where the four blocks sit in the payload. See SAVE-FORMAT.md: every global
 * array in CMVS is split, the low half riding in the slot save and the high
 * half here. */
#define CMVS_SYS_FLAGS_OFF     0x0000
#define CMVS_SYS_FLAGS_SIZE    0x7F00   /* flag bytes 0x100..0x7FFF */
#define CMVS_SYS_INTS_OFF      0x7F00
#define CMVS_SYS_INTS_SIZE     0x2000   /* int globals 2048..4095 */
#define CMVS_SYS_FLOATS_OFF    0x9F00
#define CMVS_SYS_FLOATS_SIZE   0x1000   /* float globals 1024..2047 */
#define CMVS_SYS_STRINGS_OFF   0xAF00   /* global strings 64..127, NUL separated */

#endif
