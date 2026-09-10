#include "lzss.h"

#include <stdlib.h>
#include <string.h>

const cmvs_lzss cmvs_lzss_csv2 = { 2048, 33, 3, 0xE0, 0x1F };
const cmvs_lzss cmvs_lzss_css1 = { 1024, 48, 2, 0xC0, 0x3F };

int cmvs_lzss_expand(const cmvs_lzss *p, const uint8_t *in, int in_size,
                     uint8_t *out, int out_size)
{
    uint8_t *ring;
    int i = 0, produced = 0, r, mask;
    const int n = p->n;

    ring = calloc((size_t) n, 1);
    if (!ring) return -1;
    r = n - p->f;

    while (i < in_size) {
        int flags = in[i++];
        for (mask = 0; mask < 8; mask++) {
            if (i >= in_size) goto done;
            if (flags & (1 << mask)) {
                uint8_t c = in[i++];
                if (produced >= out_size) goto overflow;
                out[produced++] = c;
                ring[r] = c;
                r = (r + 1) & (n - 1);
            } else {
                int b0, b1, pos, len, k;
                if (i + 1 >= in_size) goto done;
                b0 = in[i++];
                b1 = in[i++];
                pos = b0 | (int) ((b1 & p->pos_high) << p->pos_shift);
                len = (int) (b1 & p->len_mask) + 2;
                for (k = 0; k < len; k++) {
                    uint8_t c = ring[(pos + k) & (n - 1)];
                    if (produced >= out_size) goto overflow;
                    out[produced++] = c;
                    ring[r] = c;
                    r = (r + 1) & (n - 1);
                }
            }
        }
    }
done:
    free(ring);
    return produced;
overflow:
    free(ring);
    return -1;
}

/*
 * The compressor is Okumura's, transcribed. Its three trees make one binary
 * search tree per first byte, so the longest match at every position is found
 * exactly rather than approximated - and it is the exactness, not the speed,
 * that matters here: a different match choice compresses just as well and
 * produces a different file, which would no longer be byte-compatible.
 */
typedef struct {
    const cmvs_lzss *p;
    uint8_t *text;      /* n + f - 1 */
    int *lson, *rson, *dad;
    int match_length, match_position;
    int nil;
} tree;

static void insert_node(tree *t, int r)
{
    const int n = t->p->n, f = t->p->f, nil = t->nil;
    int cmp = 1, p = n + 1 + t->text[r];

    t->rson[r] = t->lson[r] = nil;
    t->match_length = 0;
    for (;;) {
        int i;
        if (cmp >= 0) {
            if (t->rson[p] != nil) { p = t->rson[p]; }
            else { t->rson[p] = r; t->dad[r] = p; return; }
        } else {
            if (t->lson[p] != nil) { p = t->lson[p]; }
            else { t->lson[p] = r; t->dad[r] = p; return; }
        }
        for (i = 1; i < f; i++) {
            cmp = t->text[r + i] - t->text[p + i];
            if (cmp != 0) break;
        }
        if (i > t->match_length) {
            t->match_position = p;
            t->match_length = i;
            if (i >= f) break;
        }
    }
    t->dad[r] = t->dad[p];
    t->lson[r] = t->lson[p];
    t->rson[r] = t->rson[p];
    t->dad[t->lson[p]] = r;
    t->dad[t->rson[p]] = r;
    if (t->rson[t->dad[p]] == p) t->rson[t->dad[p]] = r;
    else t->lson[t->dad[p]] = r;
    t->dad[p] = nil;
}

static void delete_node(tree *t, int p)
{
    const int nil = t->nil;
    int q;

    if (t->dad[p] == nil) return;
    if (t->rson[p] == nil) {
        q = t->lson[p];
    } else if (t->lson[p] == nil) {
        q = t->rson[p];
    } else {
        q = t->lson[p];
        if (t->rson[q] != nil) {
            while (t->rson[q] != nil) q = t->rson[q];
            t->rson[t->dad[q]] = t->lson[q];
            t->dad[t->lson[q]] = t->dad[q];
            t->lson[q] = t->lson[p];
            t->dad[t->lson[p]] = q;
        }
        t->rson[q] = t->rson[p];
        t->dad[t->rson[p]] = q;
    }
    t->dad[q] = t->dad[p];
    if (t->rson[t->dad[p]] == p) t->rson[t->dad[p]] = q;
    else t->lson[t->dad[p]] = q;
    t->dad[p] = nil;
}

uint8_t *cmvs_lzss_compress(const cmvs_lzss *p, const uint8_t *in, int in_size,
                            int *out_size)
{
    const int n = p->n, f = p->f, nil = p->n;
    tree t;
    uint8_t code[17];
    uint8_t *out;
    int cap, used = 0, cp = 1, mask = 1;
    int src = 0, s = 0, r, len = 0, i;

    if (out_size) *out_size = 0;
    /* Worst case is a flag byte for every eight literals; the +64 covers the
     * partial group and keeps a zero-length input from a zero-size malloc. */
    cap = in_size + in_size / 8 + 64;
    out = malloc((size_t) cap);
    t.p = p;
    t.text = malloc((size_t) (n + f - 1));
    t.lson = malloc(sizeof(int) * (size_t) (n + 1));
    t.rson = malloc(sizeof(int) * (size_t) (n + 257));
    t.dad = malloc(sizeof(int) * (size_t) (n + 1));
    t.nil = nil;
    if (!out || !t.text || !t.lson || !t.rson || !t.dad) {
        free(out); free(t.text); free(t.lson); free(t.rson); free(t.dad);
        return NULL;
    }
    /* The ring is zero for its first n - f bytes, which is what the engine's
     * own memset leaves and what a match into unwritten space must find. */
    memset(t.text, 0, (size_t) (n + f - 1));
    for (i = n + 1; i < n + 257; i++) t.rson[i] = nil;
    for (i = 0; i < n; i++) t.dad[i] = nil;
    t.match_length = t.match_position = 0;

    code[0] = 0;
    r = n - f;
    while (len < f && src < in_size) t.text[r + len++] = in[src++];
    if (len == 0) {
        free(t.text); free(t.lson); free(t.rson); free(t.dad);
        if (out_size) *out_size = 0;
        return out;
    }
    for (i = 1; i <= f; i++) insert_node(&t, r - i);
    insert_node(&t, r);

    for (;;) {
        int last;
        if (t.match_length > len) t.match_length = len;
        if (t.match_length <= 1) {           /* THRESHOLD */
            t.match_length = 1;
            code[0] |= (uint8_t) mask;
            code[cp++] = t.text[r];
        } else {
            code[cp++] = (uint8_t) (t.match_position & 0xFF);
            code[cp++] = (uint8_t) (((t.match_position >> p->pos_shift) & p->pos_high)
                                    | (unsigned) (t.match_length - 2));
        }
        mask = (mask << 1) & 0xFF;
        if (mask == 0) {
            memcpy(out + used, code, (size_t) cp);
            used += cp;
            code[0] = 0;
            cp = 1;
            mask = 1;
        }
        last = t.match_length;
        for (i = 0; i < last && src < in_size; i++) {
            uint8_t c = in[src++];
            delete_node(&t, s);
            t.text[s] = c;
            if (s < f - 1) t.text[s + n] = c;
            s = (s + 1) & (n - 1);
            r = (r + 1) & (n - 1);
            insert_node(&t, r);
        }
        while (i++ < last) {
            delete_node(&t, s);
            s = (s + 1) & (n - 1);
            r = (r + 1) & (n - 1);
            if (--len) insert_node(&t, r);
        }
        if (len <= 0) break;
    }
    if (cp > 1) {
        memcpy(out + used, code, (size_t) cp);
        used += cp;
    }
    free(t.text); free(t.lson); free(t.rson); free(t.dad);
    if (out_size) *out_size = used;
    return out;
}
