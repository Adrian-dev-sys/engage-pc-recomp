// Unity asset bundles for the Cobalt mod merge (unityfs.h). Written from the format as UnityPy and AssetStudio
// document it; the LZMA decoder follows the LZMA specification (lzma-specification.txt in the LZMA SDK).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unityfs.h"

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v; }
static void put_be64(uint8_t *p, uint64_t v) { put_be32(p, v >> 32), put_be32(p + 4, (uint32_t)v); }
static void put_le32(uint8_t *p, uint32_t v) { p[0] = v, p[1] = v >> 8, p[2] = v >> 16, p[3] = v >> 24; }
static void put_le64(uint8_t *p, uint64_t v) { put_le32(p, (uint32_t)v), put_le32(p + 4, v >> 32); }

// ---- LZ4 block
int lz4_unpack(const uint8_t *s, size_t n, uint8_t *d, size_t out) {
    size_t i = 0, o = 0;
    while (i < n) {
        unsigned tok = s[i++];
        size_t lit = tok >> 4;
        if (lit == 15) {
            unsigned b;
            do {
                if (i >= n) return 0;
                b = s[i++], lit += b;
            } while (b == 255);
        }
        if (lit > n - i || lit > out - o) return 0;
        memcpy(d + o, s + i, lit), i += lit, o += lit;
        if (i >= n) break;
        if (n - i < 2) return 0;
        size_t off = s[i] | s[i + 1] << 8, ml = tok & 15;
        i += 2;
        if (ml == 15) {
            unsigned b;
            do {
                if (i >= n) return 0;
                b = s[i++], ml += b;
            } while (b == 255);
        }
        ml += 4;
        if (!off || off > o || ml > out - o) return 0;
        for (size_t k = 0; k < ml; k++, o++) d[o] = d[o - off];
    }
    return o == out;
}

// ---- LZMA (LZMA1, known output size)
typedef struct { const uint8_t *p, *end; uint32_t range, code; int bad; } Rc;

static void rc_norm(Rc *r) {
    if (r->range < (1u << 24)) {
        r->range <<= 8;
        r->code = r->code << 8 | (r->p < r->end ? *r->p++ : (r->bad = 1, 0));
    }
}
static int rc_bit(Rc *r, uint16_t *p) {
    uint32_t bound = (r->range >> 11) * *p;
    int b;
    if (r->code < bound) r->range = bound, *p += (2048 - *p) >> 5, b = 0;
    else r->range -= bound, r->code -= bound, *p -= *p >> 5, b = 1;
    rc_norm(r);
    return b;
}
static uint32_t rc_direct(Rc *r, int n) {
    uint32_t v = 0;
    while (n--) {
        r->range >>= 1;
        uint32_t t = r->code >= r->range;
        if (t) r->code -= r->range;
        v = v << 1 | t;
        rc_norm(r);
    }
    return v;
}
static uint32_t tree(Rc *r, uint16_t *p, int bits) {
    uint32_t m = 1;
    for (int i = 0; i < bits; i++) m = m << 1 | rc_bit(r, &p[m]);
    return m - (1u << bits);
}
static uint32_t tree_rev(Rc *r, uint16_t *p, int bits) {
    uint32_t m = 1, v = 0;
    for (int i = 0; i < bits; i++) {
        uint32_t b = rc_bit(r, &p[m]);
        m = m << 1 | b, v |= b << i;
    }
    return v;
}

typedef struct { uint16_t choice, choice2, low[16][8], mid[16][8], high[256]; } LenCoder;

static uint32_t len_dec(Rc *r, LenCoder *l, unsigned ps) {
    if (!rc_bit(r, &l->choice)) return tree(r, l->low[ps], 3);
    if (!rc_bit(r, &l->choice2)) return 8 + tree(r, l->mid[ps], 3);
    return 16 + tree(r, l->high, 8);
}

static void probs_init(uint16_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) p[i] = 1024;
}

int lzma_unpack(const uint8_t *src, size_t n, uint8_t *d, size_t out) {
    if (n < 5 || src[0] >= 9 * 5 * 5) return 0;
    unsigned lc = src[0] % 9, lp = src[0] / 9 % 5, pb = src[0] / 45;
    struct {
        uint16_t is_match[12][16], is_rep[12], g0[12], g1[12], g2[12], rep0_long[12][16];
        uint16_t slot[4][64], pos[115], align[16];
        LenCoder len, rep_len;
    } *m = malloc(sizeof *m);
    uint16_t *lit = malloc(sizeof(uint16_t) * 0x300u << (lc + lp));
    if (!m || !lit) {
        free(m), free(lit);
        return 0;
    }
    probs_init((uint16_t *)m, sizeof *m / 2);
    probs_init(lit, (size_t)0x300 << (lc + lp));
    Rc r = {src + 5, src + n, 0xFFFFFFFFu, 0, 0};
    for (int i = 0; i < 5; i++) r.code = r.code << 8 | (r.p < r.end ? *r.p++ : 0);
    uint32_t rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0;
    unsigned state = 0;
    size_t o = 0;
    int ok = 1;
    while (o < out && ok && !r.bad) {
        unsigned ps = o & ((1u << pb) - 1);
        if (!rc_bit(&r, &m->is_match[state][ps])) {
            unsigned prev = o ? d[o - 1] : 0;
            uint16_t *p = lit + 0x300 * (((o & ((1u << lp) - 1)) << lc) + (prev >> (8 - lc)));
            unsigned sym = 1;
            if (state >= 7) {
                if (rep0 >= o) { ok = 0; break; }
                unsigned mb = d[o - rep0 - 1];
                do {
                    unsigned bit_m = mb >> 7 & 1;
                    mb <<= 1;
                    unsigned b = rc_bit(&r, &p[0x100 + (bit_m << 8) + sym]);
                    sym = sym << 1 | b;
                    if (bit_m != b) break;
                } while (sym < 0x100);
            }
            while (sym < 0x100) sym = sym << 1 | rc_bit(&r, &p[sym]);
            d[o++] = (uint8_t)sym;
            state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
            continue;
        }
        uint32_t len;
        if (rc_bit(&r, &m->is_rep[state])) {
            if (!o) { ok = 0; break; }
            if (!rc_bit(&r, &m->g0[state])) {
                if (!rc_bit(&r, &m->rep0_long[state][ps])) {
                    state = state < 7 ? 9 : 11;
                    if (rep0 >= o) { ok = 0; break; }
                    d[o] = d[o - rep0 - 1], o++;
                    continue;
                }
            } else {
                uint32_t dist;
                if (!rc_bit(&r, &m->g1[state])) dist = rep1;
                else {
                    if (!rc_bit(&r, &m->g2[state])) dist = rep2;
                    else dist = rep3, rep3 = rep2;
                    rep2 = rep1;
                }
                rep1 = rep0, rep0 = dist;
            }
            len = len_dec(&r, &m->rep_len, ps);
            state = state < 7 ? 8 : 11;
        } else {
            rep3 = rep2, rep2 = rep1, rep1 = rep0;
            len = len_dec(&r, &m->len, ps);
            state = state < 7 ? 7 : 10;
            uint32_t slot = tree(&r, m->slot[len < 3 ? len : 3], 6);
            if (slot < 4) rep0 = slot;
            else {
                int nd = (int)(slot >> 1) - 1;
                rep0 = (2 | (slot & 1)) << nd;
                if (slot < 14) rep0 += tree_rev(&r, m->pos + rep0 - slot, nd);
                else rep0 += rc_direct(&r, nd - 4) << 4, rep0 += tree_rev(&r, m->align, 4);
                if (rep0 == 0xFFFFFFFFu) break;   // end marker
            }
        }
        len += 2;
        if (rep0 >= o || len > out - o) { ok = 0; break; }
        for (uint32_t k = 0; k < len; k++, o++) d[o] = d[o - rep0 - 1];
    }
    free(m), free(lit);
    return ok && !r.bad && o == out;
}

// ---- UnityFS
static int unpack(const uint8_t *s, size_t n, uint32_t flags, uint8_t *d, size_t out) {
    switch (flags & 0x3f) {
    case 0: return n == out ? (memcpy(d, s, n), 1) : 0;
    case 1: return lzma_unpack(s, n, d, out);
    case 2: case 3: return lz4_unpack(s, n, d, out);
    default: return 0;
    }
}

static size_t cstr_end(const uint8_t *b, size_t n, size_t o) {   // index after the NUL, 0 when there is none
    const uint8_t *z = o < n ? memchr(b + o, 0, n - o) : NULL;
    return z ? (size_t)(z - b) + 1 : 0;
}

typedef struct { uint8_t *info; size_t info_n, data_at; } Hdr;

// The decompressed block / directory info and where the blocks start.
static int header(const uint8_t *f, size_t n, Hdr *h) {
    if (n < 8 || memcmp(f, "UnityFS", 8)) return 0;
    size_t o = 8;
    if (o + 4 > n) return 0;
    uint32_t ver = be32(f + o);
    o += 4;
    if (!(o = cstr_end(f, n, o)) || !(o = cstr_end(f, n, o)) || o + 20 > n) return 0;
    uint32_t csz = be32(f + o + 8), usz = be32(f + o + 12), flags = be32(f + o + 16);
    o += 20;
    if (ver >= 7) o = (o + 15) & ~(size_t)15;
    const uint8_t *src;
    if (flags & 0x80) {
        if (csz > n) return 0;
        src = f + n - csz;
    } else {
        if (csz > n - o) return 0;
        src = f + o, o += csz;
    }
    if (flags & 0x200) o = (o + 15) & ~(size_t)15;
    h->info = malloc(usz ? usz : 1);
    if (!h->info || !unpack(src, csz, flags, h->info, usz)) {
        free(h->info);
        return 0;
    }
    h->info_n = usz, h->data_at = o;
    return 1;
}

int ufs_node_names(const uint8_t *f, size_t n, char (*names)[128], int cap) {
    Hdr h;
    if (!header(f, n, &h)) return -1;
    size_t p = 16, k = 0;
    int count = -1;
    if (p + 4 <= h.info_n) {
        p += 4 + 10 * (size_t)be32(h.info + p);
        if (p + 4 <= h.info_n) {
            uint32_t nn = be32(h.info + p);
            p += 4, count = 0;
            for (uint32_t i = 0; i < nn && p + 20 <= h.info_n; i++) {
                size_t e = cstr_end(h.info, h.info_n, p + 20);
                if (!e) break;
                if ((int)k < cap) snprintf(names[k++], 128, "%s", (const char *)h.info + p + 20);
                count++, p = e;
            }
        }
    }
    free(h.info);
    return count < 0 ? -1 : (int)k;
}

int ufs_read(const uint8_t *f, size_t n, Ufs *u) {
    memset(u, 0, sizeof *u);
    Hdr h;
    if (!header(f, n, &h)) return 0;
    int ok = 0;
    size_t p = 16;
    if (p + 4 > h.info_n) goto done;
    uint32_t nb = be32(h.info + p);
    p += 4;
    if ((size_t)nb * 10 > h.info_n - p) goto done;
    size_t total = 0, in = h.data_at;
    for (uint32_t i = 0; i < nb; i++) total += be32(h.info + p + 10 * i);
    u->data = malloc(total ? total : 1);
    if (!u->data) goto done;
    for (uint32_t i = 0; i < nb; i++, p += 10) {
        uint32_t us = be32(h.info + p), cs = be32(h.info + p + 4);
        uint16_t fl = be16(h.info + p + 8);
        if (cs > n - in || !unpack(f + in, cs, fl, u->data + u->size, us)) goto done;
        in += cs, u->size += us;
    }
    if (p + 4 > h.info_n) goto done;
    uint32_t nn = be32(h.info + p);
    p += 4;
    u->nodes = calloc(nn ? nn : 1, sizeof *u->nodes);
    if (!u->nodes) goto done;
    for (uint32_t i = 0; i < nn; i++) {
        if (p + 20 > h.info_n) goto done;
        UfsNode *nd = &u->nodes[i];
        nd->offset = be64(h.info + p), nd->size = be64(h.info + p + 8), nd->flags = be32(h.info + p + 16);
        size_t e = cstr_end(h.info, h.info_n, p + 20);
        if (!e || nd->offset > u->size || nd->size > u->size - nd->offset) goto done;
        snprintf(nd->name, sizeof nd->name, "%s", (const char *)h.info + p + 20);
        p = e, u->nnodes++;
    }
    ok = 1;
done:
    free(h.info);
    if (!ok) ufs_free(u);
    return ok;
}

void ufs_free(Ufs *u) {
    free(u->data), free(u->nodes);
    memset(u, 0, sizeof *u);
}

const UfsNode *ufs_serialized(const Ufs *u) {
    for (int i = 0; i < u->nnodes; i++)
        if (!strchr(u->nodes[i].name, '.')) return &u->nodes[i];
    return NULL;
}

uint8_t *ufs_write(const char *cab, const uint8_t *sfile, size_t n, size_t *out_n) {
    size_t cl = strlen(cab) + 1, info_n = 16 + 4 + 10 + 4 + 20 + cl;
    static const char hdr[] = "UnityFS\0\0\0\0\x07" "5.x.x\0" "2020.3.18f1";
    size_t hl = sizeof hdr + 20, pad = (16 - hl % 16) % 16, total = hl + pad + info_n + n;
    uint8_t *o = calloc(1, total);
    if (!o) return NULL;
    memcpy(o, hdr, sizeof hdr);
    put_be64(o + sizeof hdr, total);
    put_be32(o + sizeof hdr + 8, (uint32_t)info_n), put_be32(o + sizeof hdr + 12, (uint32_t)info_n);
    put_be32(o + sizeof hdr + 16, 0x40);   // block info and directory together, stored
    uint8_t *i = o + hl + pad;
    put_be32(i + 16, 1);
    put_be32(i + 20, (uint32_t)n), put_be32(i + 24, (uint32_t)n);   // one stored block (flags 0)
    put_be32(i + 30, 1);
    put_be64(i + 34, 0), put_be64(i + 42, n), put_be32(i + 50, 4);   // node: a serialized file
    memcpy(i + 54, cab, cl);
    memcpy(i + info_n, sfile, n);
    *out_n = total;
    return o;
}

// ---- serialized files (version 22, little-endian metadata)
int sf_parse(const uint8_t *b, size_t n, Sf *s) {
    memset(s, 0, sizeof *s);
    s->b = b, s->n = n;
    if (n < 48) return 0;
    s->version = be32(b + 8);
    size_t o = 16;
    if (b[o]) return 0;   // big-endian metadata: not produced for Switch
    o += 4;
    if (s->version >= 22) s->data_offset = be64(b + o + 12), o += 28;
    else s->data_offset = be32(b + 12);
    if (s->version < 19 || s->data_offset > n) return 0;
#define NEED(k) do { if ((k) > n - o) goto bad; } while (0)
    int *types = NULL;
    if (!(o = cstr_end(b, n, o))) return 0;
    NEED(9);
    int tree = b[o + 4];
    int32_t tc = (int32_t)le32(b + o + 5);
    o += 9;
    types = calloc(tc > 0 ? (size_t)tc : 1, sizeof *types);
    if (!types || tc < 0) goto bad;
    for (int32_t i = 0; i < tc; i++) {
        NEED(7);
        int cid = (int32_t)le32(b + o);
        o += 7;
        if (cid == 114) { NEED(16); o += 16; }
        NEED(16);
        o += 16;
        if (tree) {
            NEED(8);
            uint64_t skip = 8 + (uint64_t)le32(b + o) * 32 + le32(b + o + 4);
            NEED(skip);
            o += skip;
            if (s->version >= 21) {
                NEED(4);
                uint64_t dc = 4 + 4 * (uint64_t)le32(b + o);
                NEED(dc);
                o += dc;
            }
        }
        types[i] = cid;
    }
    NEED(4);
    int32_t oc = (int32_t)le32(b + o);
    o += 4;
    if (oc < 0 || oc > 1 << 22) goto bad;
    s->objs = calloc(oc ? (size_t)oc : 1, sizeof *s->objs);
    if (!s->objs) goto bad;
    for (int32_t i = 0; i < oc; i++) {
        o = (o + 3) & ~(size_t)3;
        NEED(24);
        SfObject *x = &s->objs[i];
        x->path_id = (int64_t)le64(b + o), x->start = le64(b + o + 8), x->size = le32(b + o + 16), x->at = o;
        int32_t ti = (int32_t)le32(b + o + 20);
        if (ti < 0 || ti >= tc || x->start > n - s->data_offset || x->size > n - s->data_offset - x->start) goto bad;
        x->class_id = types[ti];
        o += 24;
        s->nobjs++;
    }
    NEED(4);
    int32_t sc = (int32_t)le32(b + o);
    o += 4;
    for (int32_t i = 0; i < sc; i++) {
        NEED(4);
        o = (o + 4 + 3) & ~(size_t)3;
        NEED(8);
        o += 8;
    }
    NEED(4);
    int32_t ec = (int32_t)le32(b + o);
    o += 4;
    if (ec < 0 || ec > 4096) goto bad;
    s->ext = calloc(ec ? (size_t)ec : 1, sizeof *s->ext);
    if (!s->ext) goto bad;
    for (int32_t i = 0; i < ec; i++) {
        if (!(o = cstr_end(b, n, o))) goto bad;
        NEED(20);
        o += 20;
        size_t e = cstr_end(b, n, o);
        if (!e) goto bad;
        snprintf(s->ext[i], sizeof s->ext[i], "%s", (const char *)b + o);
        o = e, s->next++;
    }
#undef NEED
    free(types);
    return 1;
bad:
    free(types);
    sf_free(s);
    return 0;
}

void sf_free(Sf *s) {
    free(s->objs), free(s->ext);
    s->objs = NULL, s->ext = NULL, s->nobjs = s->next = 0;
}

const SfObject *sf_find(const Sf *s, int cid) {
    for (int i = 0; i < s->nobjs; i++)
        if (s->objs[i].class_id == cid) return &s->objs[i];
    return NULL;
}

int sf_text_asset(const Sf *s, const SfObject *x, const char **name, uint32_t *name_len, const uint8_t **data,
                  uint32_t *len) {
    const uint8_t *p = s->b + s->data_offset + x->start;
    uint32_t sz = x->size;
    if (sz < 8) return 0;
    uint32_t nl = le32(p), at = (4 + nl + 3) & ~3u;
    if (nl > sz - 8 || at + 4 > sz) return 0;
    uint32_t dl = le32(p + at);
    if (dl > sz - at - 4) return 0;
    *name = (const char *)p + 4, *name_len = nl, *data = p + at + 4, *len = dl;
    return 1;
}

static int by_start(const void *a, const void *b) {
    const SfObject *x = *(SfObject *const *)a, *y = *(SfObject *const *)b;
    return (x->start > y->start) - (x->start < y->start);
}

uint8_t *sf_with_text_asset(const Sf *s, const SfObject *obj, const char *name, uint32_t nl, const uint8_t *data,
                            size_t len, size_t *out_n) {
    size_t ta = ((4 + nl + 3) & ~(size_t)3) + 4 + ((len + 3) & ~(size_t)3), total = s->data_offset;
    SfObject **order = malloc(sizeof *order * (s->nobjs ? s->nobjs : 1));
    if (!order) return NULL;
    for (int i = 0; i < s->nobjs; i++) order[i] = &s->objs[i];
    qsort(order, s->nobjs, sizeof *order, by_start);
    for (int i = 0; i < s->nobjs; i++) total = ((total + 7) & ~(size_t)7) + (order[i] == obj ? ta : order[i]->size);
    uint8_t *o = calloc(1, total);
    if (!o) {
        free(order);
        return NULL;
    }
    memcpy(o, s->b, s->data_offset);
    size_t at = 0;
    for (int i = 0; i < s->nobjs; i++) {
        const SfObject *x = order[i];
        at = (at + 7) & ~(size_t)7;
        uint8_t *dst = o + s->data_offset + at;
        size_t sz;
        if (x == obj) {
            put_le32(dst, nl), memcpy(dst + 4, name, nl);
            size_t p = (4 + nl + 3) & ~(size_t)3;
            put_le32(dst + p, (uint32_t)len), memcpy(dst + p + 4, data, len);
            sz = ta;
        } else {
            memcpy(dst, s->b + s->data_offset + x->start, x->size);
            sz = x->size;
        }
        put_le64(o + x->at + 8, at), put_le32(o + x->at + 16, (uint32_t)sz);
        at += sz;
    }
    free(order);
    if (s->version >= 22) put_be64(o + 24, total);
    else put_be32(o + 4, (uint32_t)total);
    *out_n = total;
    return o;
}

int sf_container_last(const Sf *s, const SfObject *x, char *out, size_t cap) {
    const uint8_t *p = s->b + s->data_offset + x->start;
    size_t sz = x->size, o = 0;
#define NEED(k) do { if ((k) > sz - o) return 0; } while (0)
    NEED(4);
    o += 4 + le32(p);
    o = (o + 3) & ~(size_t)3;
    NEED(4);
    o += 4 + 12 * (uint64_t)le32(p + o);
    NEED(4);
    uint32_t cc = le32(p + o);
    o += 4;
    int found = 0;
    for (uint32_t i = 0; i < cc; i++) {
        NEED(4);
        uint32_t l = le32(p + o);
        NEED(4 + (uint64_t)l);
        if (l < cap) memcpy(out, p + o + 4, l), out[l] = 0, found = 1;
        o = (o + 4 + l + 3) & ~(size_t)3;
        NEED(20);
        o += 20;
    }
#undef NEED
    return found;
}
