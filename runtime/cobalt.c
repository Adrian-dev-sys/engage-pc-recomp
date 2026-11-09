// Cobalt data mods (cobalt.h). Cobalt (github.com/Raytwo/Cobalt) does this at run time with hooks; the recompiled
// game cannot run its ARM64 plugin, so the same results are built here once, as files:
// - new bundles under Data/StreamingAssets/aa/Switch: a bundle entry and a prefab entry per bundle are added to the
//   Addressables catalog (Data/StreamingAssets/aa/catalog.bundle), like Cobalt's catalog/synthesize.rs: the address
//   is the bundle's last container path without its Addressables prefix and extension, the dependencies are the
//   bundle itself and the bundles that hold its external CABs;
// - patches/xml/<Name>.xml: each mod's full sheet is diffed against the game's gamedata XML and the changes applied
//   in mod order, the rules of cobalt_xml_merge (DivineDragonFanClub): tags are the units, an insert keeps every
//   mod's rows, a replaced opening tag of the same name is overwritten by the later mod;
// - patches/msbt/message/[<lang>/<region>/]<file>.txt: astra-msbt text ([LABEL] then its text) adds or replaces
//   labels in the message bundle; a file without a language folder applies to every language.
// Mods apply bottom of the F1 > Mods order first, so the top mod wins. The result is cached under <profile>/cobalt
// and rebuilt when any mod file or the order changes.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cobalt.h"
#include "fs.h"
#include "host.h"
#include "mods.h"
#include "platform/platform.h"
#include "romfs_config.h"
#include "sha256.h"
#include "unityfs.h"

#define SA "Data/StreamingAssets"
#define BUNDLE_DIR SA "/aa/Switch"
#define CATALOG SA "/aa/catalog.bundle"
#define RT "{UnityEngine.AddressableAssets.Addressables.RuntimePath}/Switch/"
#define BAKE_VERSION "cobalt-bake 1\n"
#define MAX_COBALT 512

static char cache_dir[2048];
static int order[MAX_COBALT], norder;   // mods_at indices of the enabled Cobalt mods, bottom of the list first

// ---- small helpers
typedef struct { uint8_t *p; size_t n, cap; } Buf;

static void buf_add(Buf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap : 4096;
        while (c < b->n + n) c *= 2;
        uint8_t *p = realloc(b->p, c);
        if (!p) rt_fatal("cobalt: out of memory");
        b->p = p, b->cap = c;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void buf_str(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }
static void buf_i32(Buf *b, int32_t v) {
    uint8_t x[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    buf_add(b, x, 4);
}
static int32_t rd32(const uint8_t *p) { return (int32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }
static void wr32(uint8_t *p, int32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }

static void lower_copy(char *d, const char *s, size_t cap) {
    size_t i = 0;
    for (; s[i] && i + 1 < cap; i++) d[i] = (char)(s[i] >= 'A' && s[i] <= 'Z' ? s[i] + 32 : s[i]);
    d[i] = 0;
}

static int ends_ci(const char *s, const char *t) {
    size_t n = strlen(s), k = strlen(t);
    return n >= k && !strcasecmp(s + n - k, t);
}

// Every file under dir (recursive), with its path relative to dir; junk (.*, __MACOSX) left out.
typedef void (*WalkFn)(const char *file, const char *rel, uint64_t size, void *ctx);
typedef struct { const char *dir, *rel; WalkFn fn; void *ctx; } Walk;

static void walk(const char *dir, const char *rel, WalkFn fn, void *ctx);
static int walk_cb(const char *name, int kind, uint64_t size, void *ctx) {
    const Walk *w = ctx;
    if (name[0] == '.' || !strcmp(name, "__MACOSX")) return 1;
    char file[4200], rel[4200];
    if (snprintf(file, sizeof file, "%s/%s", w->dir, name) >= (int)sizeof file ||
        snprintf(rel, sizeof rel, "%s%s%s", w->rel, *w->rel ? "/" : "", name) >= (int)sizeof rel)
        return 1;
    if (kind == PLAT_FS_DIR) walk(file, rel, w->fn, w->ctx);
    else if (kind == PLAT_FS_FILE) w->fn(file, rel, size, w->ctx);
    return 1;
}
static void walk(const char *dir, const char *rel, WalkFn fn, void *ctx) {
    Walk w = {dir, rel, fn, ctx};
    if (plat_dir_exists(dir)) plat_fs_list(dir, walk_cb, &w);
}

static void mod_sub(int k, const char *sub, char *out, size_t cap) { snprintf(out, cap, "%s/%s", mods_at(k)->romfs, sub); }

// ---- base64
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static uint8_t *b64_decode(const char *s, size_t n, size_t *out_n) {
    static int8_t t[256];
    if (!t['B']) {
        memset(t, -1, sizeof t);
        for (int i = 0; i < 64; i++) t[(uint8_t)B64[i]] = (int8_t)i;
    }
    uint8_t *o = malloc(n / 4 * 3 + 3);
    if (!o) return NULL;
    size_t k = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        int v = t[(uint8_t)s[i]];
        if (v < 0) continue;   // '=' and anything else
        acc = acc << 6 | (uint32_t)v, bits += 6;
        if (bits >= 8) bits -= 8, o[k++] = (uint8_t)(acc >> bits);
    }
    *out_n = k;
    return o;
}

static void b64_encode(Buf *b, const uint8_t *d, size_t n) {
    char q[4];
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        q[0] = B64[v >> 18], q[1] = B64[v >> 12 & 63];
        q[2] = i + 1 < n ? B64[v >> 6 & 63] : '=', q[3] = i + 2 < n ? B64[v & 63] : '=';
        buf_add(b, q, 4);
    }
}

// ---- reading and writing bundles' TextAssets
typedef struct { uint8_t *file; Ufs u; Sf sf; const SfObject *ta; const UfsNode *node; const char *name;
                 uint32_t name_len; const uint8_t *data; uint32_t len; } TextBundle;

static void tb_free(TextBundle *t) {
    sf_free(&t->sf), ufs_free(&t->u), free(t->file);
    memset(t, 0, sizeof *t);
}

// The TextAsset of RomFS bundle path as the mods leave it; 1 on success.
static int tb_open(const char *path, TextBundle *t) {
    memset(t, 0, sizeof *t);
    uint64_t n = 0;
    t->file = romfs_base_read(path, &n);
    if (!t->file) return 0;
    if (ufs_read(t->file, n, &t->u) && (t->node = ufs_serialized(&t->u)) &&
        sf_parse(t->u.data + t->node->offset, t->node->size, &t->sf) && (t->ta = sf_find(&t->sf, 49)) &&
        sf_text_asset(&t->sf, t->ta, &t->name, &t->name_len, &t->data, &t->len))
        return 1;
    rt_log("cobalt: %s is not a bundle with a TextAsset", path);
    tb_free(t);
    return 0;
}

static char outputs[4096][256];   // RomFS paths written this start
static int noutputs;

static void out_path(const char *rom, char *out, size_t cap) { snprintf(out, cap, "%s/out/%s", cache_dir, rom); }

// Writes t with its TextAsset replaced by data as cache file for RomFS path rom.
static int tb_write(const TextBundle *t, const char *rom, const uint8_t *data, size_t len) {
    size_t sn = 0, bn = 0;
    uint8_t *sf = sf_with_text_asset(&t->sf, t->ta, t->name, t->name_len, data, len, &sn);
    uint8_t *b = sf ? ufs_write(t->node->name, sf, sn, &bn) : NULL;
    free(sf);
    char path[2600];
    out_path(rom, path, sizeof path);
    char dir[2600];
    snprintf(dir, sizeof dir, "%s", path);
    char *cut = strrchr(dir, '/');
    if (cut) *cut = 0, plat_file_mkdir_p(dir);
    int ok = b && plat_file_write(path, b, bn);
    free(b);
    if (!ok) {
        rt_log("cobalt: cannot write %s", path);
        return 0;
    }
    if (noutputs < (int)(sizeof outputs / sizeof *outputs)) snprintf(outputs[noutputs++], sizeof *outputs, "%s", rom);
    return 1;
}

// ======== catalog
typedef struct { char *key; char *val; } Pair;   // a lower-case string -> value table, open addressing

typedef struct { Pair *v; size_t cap, n; } Map;

static uint64_t fnv(const char *s, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 1099511628211ull;
    return h;
}
static void map_put(Map *m, const char *k, const char *v) {
    if ((m->n + 1) * 2 > m->cap) {
        Map g = {calloc(m->cap ? m->cap * 2 : 1024, sizeof(Pair)), m->cap ? m->cap * 2 : 1024, 0};
        if (!g.v) rt_fatal("cobalt: out of memory");
        for (size_t i = 0; i < m->cap; i++)
            if (m->v[i].key) {
                size_t j = fnv(m->v[i].key, strlen(m->v[i].key)) & (g.cap - 1);
                while (g.v[j].key) j = (j + 1) & (g.cap - 1);
                g.v[j] = m->v[i], g.n++;
            }
        free(m->v);
        *m = g;
    }
    size_t j = fnv(k, strlen(k)) & (m->cap - 1);
    while (m->v[j].key && strcmp(m->v[j].key, k)) j = (j + 1) & (m->cap - 1);
    if (!m->v[j].key) m->v[j].key = strdup(k), m->n++;
    else free(m->v[j].val);
    m->v[j].val = strdup(v);
}
static const char *map_get(const Map *m, const char *k) {
    if (!m->cap) return NULL;
    size_t j = fnv(k, strlen(k)) & (m->cap - 1);
    while (m->v[j].key) {
        if (!strcmp(m->v[j].key, k)) return m->v[j].val;
        j = (j + 1) & (m->cap - 1);
    }
    return NULL;
}
static void map_free(Map *m) {
    for (size_t i = 0; i < m->cap; i++) free(m->v[i].key), free(m->v[i].val);
    free(m->v);
    memset(m, 0, sizeof *m);
}

typedef struct {
    const char *json;
    size_t json_n;
    size_t ids_end;                // offset of the m_InternalIds array's ']'
    size_t ks, ke, bs, be, es, ee, xs, xe;   // the four base64 strings' value ranges
    Buf K, B, E, X;                // decoded, growing
    int32_t nids, nbuckets, nentries;
    Map id;                        // lower-case internal id -> its index
    int32_t *first_entry;          // internal id index -> its first entry, -1
    size_t first_cap;
    Buf new_ids;                   // ',"<id>"' for each added internal id
    int32_t template_entry;
} Catalog;

static int json_string_value(const char *j, size_t n, const char *field, size_t *s, size_t *e) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":\"", field);
    const char *p = strstr(j, pat);   // the catalog has no NUL before its end
    if (!p || (size_t)(p - j) >= n) return 0;
    *s = (size_t)(p - j) + strlen(pat);
    const char *q = memchr(j + *s, '"', n - *s);
    if (!q) return 0;
    *e = (size_t)(q - j);
    return 1;
}

static void cat_add_id_index(Catalog *c, const char *id, int32_t i) {
    char lo[1024], num[16];
    lower_copy(lo, id, sizeof lo);
    snprintf(num, sizeof num, "%d", i);
    map_put(&c->id, lo, num);
}

static int32_t cat_id(const Catalog *c, const char *id) {
    char lo[1024];
    lower_copy(lo, id, sizeof lo);
    const char *v = map_get(&c->id, lo);
    return v ? atoi(v) : -1;
}

static int cat_load(Catalog *c, const uint8_t *json, size_t n) {
    memset(c, 0, sizeof *c);
    char *copy = malloc(n + 1);   // NUL-terminated for strstr
    if (!copy) return 0;
    memcpy(copy, json, n), copy[n] = 0;
    c->json = copy, c->json_n = n;
    const char *j = c->json;
    const char *ids = strstr(j, "\"m_InternalIds\":[");
    if (!ids || !json_string_value(j, n, "m_KeyDataString", &c->ks, &c->ke) ||
        !json_string_value(j, n, "m_BucketDataString", &c->bs, &c->be) ||
        !json_string_value(j, n, "m_EntryDataString", &c->es, &c->ee) ||
        !json_string_value(j, n, "m_ExtraDataString", &c->xs, &c->xe))
        return 0;
    size_t p = (size_t)(ids - j) + strlen("\"m_InternalIds\":[");
    char id[1024];
    while (p < n && j[p] != ']') {
        if (j[p] == '"') {
            size_t k = 0;
            for (p++; p < n && j[p] != '"'; p++) {
                char ch = j[p];
                if (ch == '\\' && p + 1 < n) ch = j[++p];
                if (k + 1 < sizeof id) id[k++] = ch;
            }
            id[k] = 0;
            cat_add_id_index(c, id, c->nids++);
        }
        p++;
    }
    if (p >= n) return 0;
    c->ids_end = p;
    size_t m;
    uint8_t *d;
    d = b64_decode(j + c->ks, c->ke - c->ks, &m), buf_add(&c->K, d, m), free(d);
    d = b64_decode(j + c->bs, c->be - c->bs, &m), buf_add(&c->B, d, m), free(d);
    d = b64_decode(j + c->es, c->ee - c->es, &m), buf_add(&c->E, d, m), free(d);
    d = b64_decode(j + c->xs, c->xe - c->xs, &m), buf_add(&c->X, d, m), free(d);
    if (c->B.n < 4 || c->E.n < 4) return 0;
    c->nbuckets = rd32(c->B.p), c->nentries = rd32(c->E.p);
    if ((size_t)c->nentries * 28 + 4 > c->E.n) return 0;
    c->first_cap = (size_t)c->nids + 4096;
    c->first_entry = malloc(c->first_cap * sizeof *c->first_entry);
    if (!c->first_entry) return 0;
    for (size_t i = 0; i < c->first_cap; i++) c->first_entry[i] = -1;
    for (int32_t e = c->nentries - 1; e >= 0; e--) {
        int32_t iid = rd32(c->E.p + 4 + 28 * (size_t)e);
        if (iid >= 0 && iid < c->nids) c->first_entry[iid] = e;
    }
    int32_t t = cat_id(c, RT "fe_assets_unit/model/ubody/amr0af/c000/prefabs/ubody_amr0af_c000.bundle");
    c->template_entry = t >= 0 ? c->first_entry[t] : -1;
    return c->template_entry >= 0;
}

static void cat_free(Catalog *c) {
    free(c->K.p), free(c->B.p), free(c->E.p), free(c->X.p), free(c->new_ids.p), free(c->first_entry);
    free((char *)c->json);
    map_free(&c->id);
}

static int32_t cat_entry_of(const Catalog *c, const char *id) {
    int32_t i = cat_id(c, id);
    return i >= 0 && (size_t)i < c->first_cap ? c->first_entry[i] : -1;
}

static int32_t cat_new_id(Catalog *c, const char *id) {
    buf_str(&c->new_ids, ",\"");
    for (const char *p = id; *p; p++) {
        if (*p == '"' || *p == '\\') buf_add(&c->new_ids, "\\", 1);
        buf_add(&c->new_ids, p, 1);
    }
    buf_str(&c->new_ids, "\"");
    int32_t i = c->nids++;
    cat_add_id_index(c, id, i);
    return i;
}

static int32_t cat_key_str(Catalog *c, const char *s) {
    int32_t off = (int32_t)c->K.n;
    uint8_t t = 0;
    buf_add(&c->K, &t, 1), buf_i32(&c->K, (int32_t)strlen(s)), buf_str(&c->K, s);
    return off;
}
static int32_t cat_key_int(Catalog *c, int32_t v) {
    int32_t off = (int32_t)c->K.n;
    uint8_t t = 4;
    buf_add(&c->K, &t, 1), buf_i32(&c->K, v);
    return off;
}
static int32_t cat_bucket(Catalog *c, int32_t key, const int32_t *ents, int n) {
    buf_i32(&c->B, key), buf_i32(&c->B, n);
    for (int i = 0; i < n; i++) buf_i32(&c->B, ents[i]);
    return c->nbuckets++;
}
static int32_t cat_entry(Catalog *c, const int32_t v[7]) {
    for (int i = 0; i < 7; i++) buf_i32(&c->E, v[i]);
    return c->nentries++;
}

// Replaces "field":<value> in an ASCII JSON object (value up to the next ',' or '}').
static void json_set(char *js, size_t cap, const char *field, const char *value) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", field);
    char *p = strstr(js, pat);
    if (!p) return;
    p += strlen(pat);
    char *e = p;
    if (*e == '"') {
        e = strchr(e + 1, '"');
        if (!e) return;
        e++;
    } else {
        while (*e && *e != ',' && *e != '}') e++;
    }
    size_t tail = strlen(e), vl = strlen(value);
    if ((size_t)(p - js) + vl + tail + 1 > cap) return;
    memmove(p + vl, e, tail + 1);
    memcpy(p, value, vl);
}

// A bundle entry for rel (lower case, under aa/Switch) with the template's AssetBundleRequestOptions.
static int32_t cat_add_bundle(Catalog *c, const char *rel, uint64_t size) {
    const uint8_t *t = c->E.p + 4 + 28 * (size_t)c->template_entry;
    int32_t provider = rd32(t + 4), rtype = rd32(t + 24), x = rd32(t + 16);
    size_t o = (size_t)x + 1;
    for (int i = 0; i < 2 && o < c->X.n; i++) o += 1 + c->X.p[o];
    if (x < 0 || o + 4 > c->X.n) return -1;
    int32_t jl = rd32(c->X.p + o);
    if (jl < 0 || o + 4 + (size_t)jl > c->X.n) return -1;
    char js[2048];
    size_t k = 0;
    for (int32_t i = 0; i + 1 < jl && k + 1 < sizeof js; i += 2) js[k++] = (char)c->X.p[o + 4 + i];
    js[k] = 0;
    char v[64];
    uint8_t md[32];
    sha256(rel, strlen(rel), md);
    snprintf(v, sizeof v, "\"");
    for (int i = 0; i < 16; i++) snprintf(v + 1 + 2 * i, 3, "%02x", md[i]);
    strcat(v, "\"");
    json_set(js, sizeof js, "m_BundleName", v);
    snprintf(v, sizeof v, "%llu", (unsigned long long)size);
    json_set(js, sizeof js, "m_BundleSize", v);
    json_set(js, sizeof js, "m_Crc", "0");
    json_set(js, sizeof js, "m_Hash", "\"\"");
    int32_t dx = (int32_t)c->X.n;
    Buf head = {0};
    buf_add(&head, c->X.p + x, o - (size_t)x);
    buf_add(&c->X, head.p, head.n);
    free(head.p);
    size_t jn = strlen(js);
    buf_i32(&c->X, (int32_t)(jn * 2));
    for (size_t i = 0; i < jn; i++) {
        uint8_t u[2] = {(uint8_t)js[i], 0};
        buf_add(&c->X, u, 2);
    }
    char iid[1024];
    snprintf(iid, sizeof iid, RT "%s", rel);
    int32_t id = cat_new_id(c, iid), e = c->nentries;
    int32_t b = cat_bucket(c, cat_key_str(c, rel), &e, 1);
    int32_t row[7] = {id, provider, -1, 0, dx, b, rtype};
    cat_entry(c, row);
    if ((size_t)id < c->first_cap) c->first_entry[id] = e;
    return e;
}

static void cat_add_prefab(Catalog *c, const char *address, const char *internal, const int32_t *deps, int ndeps) {
    uint64_t h = fnv((const char *)deps, sizeof *deps * (size_t)ndeps);
    int32_t hk = (int32_t)(uint32_t)h;
    int32_t dk = cat_bucket(c, cat_key_int(c, hk), deps, ndeps);
    int32_t id = cat_new_id(c, internal), e = c->nentries;
    int32_t b = cat_bucket(c, cat_key_str(c, address), &e, 1);
    int32_t row[7] = {id, 2, dk, hk, -1, b, 4};   // BundledAssetProvider, GameObject
    cat_entry(c, row);
}

static uint8_t *cat_dump(Catalog *c, size_t *out_n) {
    wr32(c->K.p, c->nbuckets);   // the key count: Unity sizes its key array by it
    wr32(c->B.p, c->nbuckets);
    wr32(c->E.p, c->nentries);
    Buf o = {0};
    struct { size_t s, e; Buf *b; } parts[4] = {{c->ks, c->ke, &c->K}, {c->bs, c->be, &c->B},
                                                {c->es, c->ee, &c->E}, {c->xs, c->xe, &c->X}};
    // The splices in file order: the four strings and the end of m_InternalIds.
    size_t at = 0;
    for (int round = 0; round < 5; round++) {
        size_t best = (size_t)-1;
        int which = -1;
        for (int i = 0; i < 4; i++)
            if (parts[i].s >= at && parts[i].s < best) best = parts[i].s, which = i;
        if (c->ids_end >= at && c->ids_end < best) best = c->ids_end, which = 4;
        if (which < 0) break;
        buf_add(&o, c->json + at, best - at);
        if (which == 4) {
            buf_add(&o, c->new_ids.p ? c->new_ids.p : (uint8_t *)"", c->new_ids.n);
            at = best;
        } else {
            b64_encode(&o, parts[which].b->p, parts[which].b->n);
            at = parts[which].e;
            parts[which].s = (size_t)-1;
        }
        if (which == 4) c->ids_end = (size_t)-1;
    }
    buf_add(&o, c->json + at, c->json_n - at);
    *out_n = o.n;
    return o.p;
}

// ---- CAB name -> bundle path (lower case, under aa/Switch), for the dependencies of new bundles
static Map cabs;
static int cabs_base_loaded;

static void cabs_from_file(const char *file, const char *rel) {
    uint64_t n = 0;
    uint8_t *b = plat_file_read(file, &n);
    char names[16][128];
    int k = b ? ufs_node_names(b, n, names, 16) : -1;
    for (int i = 0; i < k; i++) {
        char lo[128], rl[1024];
        lower_copy(lo, names[i], sizeof lo), lower_copy(rl, rel, sizeof rl);
        map_put(&cabs, lo, rl);
    }
    free(b);
}

// The game's own bundles: read from romfs.bin's headers once, then kept in <profile>/cobalt/cabs.txt.
static void cabs_base(void) {
    if (cabs_base_loaded) return;
    cabs_base_loaded = 1;
    char path[2200];
    snprintf(path, sizeof path, "%s/cabs.txt", cache_dir);
    uint64_t n = 0;
    char *t = (char *)plat_file_read(path, &n);
    const char *want = "romfs " ROMFS_INDEX_SHA256 "\n";
    if (t && n > strlen(want) && !memcmp(t, want, strlen(want))) {
        char *p = t + strlen(want), *end = t + n;
        while (p < end) {
            char *nl = memchr(p, '\n', (size_t)(end - p)), *tab = memchr(p, '\t', (size_t)(end - p));
            if (!nl || !tab || tab > nl) break;
            *nl = *tab = 0;
            map_put(&cabs, p, tab + 1);
            p = nl + 1;
        }
        free(t);
        return;
    }
    free(t);
    double t0 = (double)plat_now_ns() / 1e9;
    PlatFile *img = plat_file_open(romfs_image_path());
    if (!img) return;
    Buf out = {0};
    buf_str(&out, want);
    uint8_t *head = malloc(256 * 1024);
    size_t pre = strlen(BUNDLE_DIR "/"), count = 0;
    for (uint64_t i = 0; head && i < romfs_file_count(); i++) {
        uint64_t off, size;
        const char *name = romfs_entry(i, &off, &size);
        if (strncmp(name, BUNDLE_DIR "/", pre) || !ends_ci(name, ".bundle")) continue;
        char names[16][128];
        int k = -1;
        for (uint32_t want_n = 4096; k < 0; want_n = 256 * 1024) {
            uint32_t got = size < want_n ? (uint32_t)size : want_n;
            if (plat_file_pread(img, head, got, off) != got) break;
            k = ufs_node_names(head, got, names, 16);
            if (k >= 0 || got == size || want_n == 256 * 1024) break;
        }
        for (int j = 0; j < k; j++) {
            char lo[128], rl[1024];
            lower_copy(lo, names[j], sizeof lo), lower_copy(rl, name + pre, sizeof rl);
            map_put(&cabs, lo, rl);
            buf_str(&out, lo), buf_add(&out, "\t", 1), buf_str(&out, rl), buf_add(&out, "\n", 1);
            count++;
        }
    }
    free(head);
    plat_file_close(img);
    plat_file_write_atomic(path, out.p, out.n);
    free(out.p);
    rt_log("cobalt: %zu CAB names of the game's bundles listed in %.1f s (%s)", count,
           (double)plat_now_ns() / 1e9 - t0, path);
}

typedef struct { char file[2600], rel[1024]; uint64_t size; } NewBundle;
typedef struct { NewBundle *v; int n, cap; } BundleList;

static void bundle_cb(const char *file, const char *rel, uint64_t size, void *ctx) {
    BundleList *l = ctx;
    if (!ends_ci(rel, ".bundle")) return;
    char lo[1024];
    lower_copy(lo, rel, sizeof lo);
    for (int i = 0; i < l->n; i++)   // a higher mod's copy (applied later) replaces a lower one's
        if (!strcmp(l->v[i].rel, lo)) {
            snprintf(l->v[i].file, sizeof l->v[i].file, "%s", file), l->v[i].size = size;
            return;
        }
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->v = realloc(l->v, sizeof *l->v * (size_t)l->cap);
        if (!l->v) rt_fatal("cobalt: out of memory");
    }
    NewBundle *b = &l->v[l->n++];
    snprintf(b->file, sizeof b->file, "%s", file), snprintf(b->rel, sizeof b->rel, "%s", lo), b->size = size;
}

static const char *const strip_pre[] = {"Assets/Share/Addressables/", "Assets/Project/Addressables/",
                                        "Assets/Share/Scenes/Map/", "Patch/Patch0/", "Patch/Patch1/",
                                        "Patch/Patch2/", "Patch/Patch3/"};
static const char *const strip_suf[] = {".prefab", ".png", ".asset", ".unity", ".fbx", ".overrideController",
                                        ".spriteatlas"};

static void bake_catalog(void) {
    BundleList l = {0};
    for (int i = 0; i < norder; i++) {
        char d[2200];
        mod_sub(order[i], BUNDLE_DIR, d, sizeof d);
        walk(d, "", bundle_cb, &l);
    }
    if (!l.n) return;
    TextBundle tb;
    if (!tb_open(CATALOG, &tb)) return;
    Catalog c;
    if (!cat_load(&c, tb.data, tb.len)) {
        rt_log("cobalt: the catalog's layout is not the one this merge knows; new bundles stay unknown to the game");
        tb_free(&tb);
        return;
    }
    for (int i = 0; i < l.n; i++) cabs_from_file(l.v[i].file, l.v[i].rel);
    int added = 0;
    for (int i = 0; i < l.n; i++) {
        NewBundle *nb = &l.v[i];
        char iid[1100];
        snprintf(iid, sizeof iid, RT "%s", nb->rel);
        if (cat_entry_of(&c, iid) >= 0) continue;   // replaces a game bundle: the catalog already has it
        uint64_t n = 0;
        uint8_t *f = plat_file_read(nb->file, &n);
        Ufs u;
        Sf sf;
        const UfsNode *node;
        const SfObject *ab;
        char cont[1024];
        if (!f || !ufs_read(f, n, &u)) {
            rt_log("cobalt: %s is not a bundle; left out of the catalog", nb->file);
            free(f);
            continue;
        }
        if (!(node = ufs_serialized(&u)) || !sf_parse(u.data + node->offset, node->size, &sf)) {
            rt_log("cobalt: %s has no serialized file; left out of the catalog", nb->file);
            ufs_free(&u), free(f);
            continue;
        }
        if (!(ab = sf_find(&sf, 142)) || !sf_container_last(&sf, ab, cont, sizeof cont)) {
            rt_log("cobalt: %s has no AssetBundle container; left out of the catalog", nb->file);
            sf_free(&sf), ufs_free(&u), free(f);
            continue;
        }
        int32_t deps[256];
        int nd = 0;
        deps[nd++] = cat_add_bundle(&c, nb->rel, nb->size);
        for (int k = 0; k < sf.next && nd < 256; k++) {
            const char *cab = strrchr(sf.ext[k], '/');
            char lo[256];
            lower_copy(lo, cab ? cab + 1 : sf.ext[k], sizeof lo);
            const char *rel = map_get(&cabs, lo);
            if (!rel) cabs_base(), rel = map_get(&cabs, lo);
            if (!rel) {
                rt_log("cobalt: %s needs %s, which no bundle holds", nb->rel, lo);
                continue;
            }
            snprintf(iid, sizeof iid, RT "%s", rel);
            int32_t e = cat_entry_of(&c, iid);
            if (e < 0) {
                rt_log("cobalt: %s needs %s, which is not in the catalog", nb->rel, rel);
                continue;
            }
            int dup = 0;
            for (int q = 0; q < nd; q++) dup |= deps[q] == e;
            if (!dup) deps[nd++] = e;
        }
        char addr[1024];
        const char *a = cont;
        for (size_t q = 0; q < sizeof strip_pre / sizeof *strip_pre; q++)
            if (!strncmp(a, strip_pre[q], strlen(strip_pre[q]))) {
                a += strlen(strip_pre[q]);
                break;
            }
        snprintf(addr, sizeof addr, "%s", a);
        for (size_t q = 0; q < sizeof strip_suf / sizeof *strip_suf; q++)
            if (ends_ci(addr, strip_suf[q])) {
                addr[strlen(addr) - strlen(strip_suf[q])] = 0;
                break;
            }
        cat_add_prefab(&c, addr, cont, deps, nd);
        added++;
        sf_free(&sf), ufs_free(&u), free(f);
    }
    if (added) {
        size_t n = 0;
        uint8_t *json = cat_dump(&c, &n);
        if (json && tb_write(&tb, CATALOG, json, n)) rt_log("cobalt: catalog: %d new bundles added", added);
        free(json);
    }
    cat_free(&c);
    tb_free(&tb);
    free(l.v);
}

// ======== XML gamedata (cobalt_xml_merge)
typedef struct { const char *p; uint32_t n; const char *np; uint32_t nn; uint64_t h; } Tok;
typedef struct { Tok *v; int n, cap; } Toks;

static void toks_push(Toks *t, Tok k) {
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 1024;
        t->v = realloc(t->v, sizeof *t->v * (size_t)t->cap);
        if (!t->v) rt_fatal("cobalt: out of memory");
    }
    t->v[t->n++] = k;
}

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Each '<' .. '>' run; comments, doctype and CDATA (<!...>) left out; text between tags dropped.
static Toks tokenize(const char *s, size_t n) {
    Toks t = {0};
    size_t last = 0;
    for (size_t p = 0; p < n; p++) {
        if (s[p] != '>') continue;
        const char *lt = memchr(s + last, '<', p + 1 - last);
        size_t start = last;
        last = p + 1;
        if (!lt) continue;
        start = (size_t)(lt - s);
        if (p > start && s[start + 1] == '!') continue;
        Tok k = {s + start, (uint32_t)(p + 1 - start), 0, 0, 0};
        const char *a = k.p, *e = k.p + k.n;
        while (e - a >= 2 && e[-2] == '/' && e[-1] == '>') e -= 2;
        while (a < e && is_ws(*a)) a++;
        while (e > a && is_ws(e[-1])) e--;
        k.np = a, k.nn = (uint32_t)(e - a), k.h = fnv(a, k.nn);
        toks_push(&t, k);
    }
    return t;
}

static int tok_eq(const Tok *a, const Tok *b) { return a->h == b->h && a->nn == b->nn && !memcmp(a->np, b->np, a->nn); }

// GNU diff's compareseq / diag (Myers' O(ND) with the middle snake), no heuristics.
typedef struct { const Tok *a, *b; char *ac, *bc; int *fd, *bd; } Diff;

static void diag(Diff *d, int xoff, int xlim, int yoff, int ylim, int *xm, int *ym) {
    int *fd = d->fd, *bd = d->bd;
    int dmin = xoff - ylim, dmax = xlim - yoff, fmid = xoff - yoff, bmid = xlim - ylim;
    int fmin = fmid, fmax = fmid, bmin = bmid, bmax = bmid, odd = (fmid - bmid) & 1;
    fd[fmid] = xoff, bd[bmid] = xlim;
    for (;;) {
        if (fmin > dmin) fd[--fmin - 1] = -1;
        else ++fmin;
        if (fmax < dmax) fd[++fmax + 1] = -1;
        else --fmax;
        for (int k = fmax; k >= fmin; k -= 2) {
            int tlo = fd[k - 1], thi = fd[k + 1], x = tlo >= thi ? tlo + 1 : thi, y = x - k;
            while (x < xlim && y < ylim && tok_eq(&d->a[x], &d->b[y])) x++, y++;
            fd[k] = x;
            if (odd && bmin <= k && k <= bmax && bd[k] <= x) {
                *xm = x, *ym = y;
                return;
            }
        }
        if (bmin > dmin) bd[--bmin - 1] = 0x7fffffff;
        else ++bmin;
        if (bmax < dmax) bd[++bmax + 1] = 0x7fffffff;
        else --bmax;
        for (int k = bmax; k >= bmin; k -= 2) {
            int tlo = bd[k - 1], thi = bd[k + 1], x = tlo < thi ? tlo : thi - 1, y = x - k;
            while (x > xoff && y > yoff && tok_eq(&d->a[x - 1], &d->b[y - 1])) x--, y--;
            bd[k] = x;
            if (!odd && fmin <= k && k <= fmax && x <= fd[k]) {
                *xm = x, *ym = y;
                return;
            }
        }
    }
}

static void compareseq(Diff *d, int xoff, int xlim, int yoff, int ylim) {
    while (xoff < xlim && yoff < ylim && tok_eq(&d->a[xoff], &d->b[yoff])) xoff++, yoff++;
    while (xlim > xoff && ylim > yoff && tok_eq(&d->a[xlim - 1], &d->b[ylim - 1])) xlim--, ylim--;
    if (xoff == xlim) {
        for (int y = yoff; y < ylim; y++) d->bc[y] = 1;
    } else if (yoff == ylim) {
        for (int x = xoff; x < xlim; x++) d->ac[x] = 1;
    } else {
        int xm, ym;
        diag(d, xoff, xlim, yoff, ylim, &xm, &ym);
        compareseq(d, xoff, xm, yoff, ym);
        compareseq(d, xm, xlim, ym, ylim);
    }
}

typedef struct { const Tok **v; int n, cap; } Refs;
static void refs_push(Refs *r, const Tok *t) {
    if (r->n == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 4;
        r->v = realloc(r->v, sizeof *r->v * (size_t)r->cap);
        if (!r->v) rt_fatal("cobalt: out of memory");
    }
    r->v[r->n++] = t;
}

typedef struct { int deleted; Refs front, back; } Slot;

// The name of a non-self-closing tag (closing ones as "/Name"), or n = 0.
static const char *block_tag(const Tok *t, uint32_t *n) {
    const char *a = t->p, *e = t->p + t->n;
    while (a < e && is_ws(*a)) a++;
    while (e > a && is_ws(e[-1])) e--;
    *n = 0;
    if (e - a < 2 || *a != '<' || a[1] == '?' || a[1] == '!' || (e[-2] == '/' && e[-1] == '>')) return NULL;
    const char *s = a + 1, *q = s;
    while (q < e && !is_ws(*q) && *q != '>') q++;
    *n = (uint32_t)(q - s);
    return s;
}

static void apply_patch(const Toks *base, Slot *arena, const Toks *patch) {
    int na = base->n, nb = patch->n;
    Diff d = {base->v, patch->v, calloc((size_t)na + 1, 1), calloc((size_t)nb + 1, 1), NULL, NULL};
    int *fdb = malloc(sizeof(int) * ((size_t)na + nb + 3)), *bdb = malloc(sizeof(int) * ((size_t)na + nb + 3));
    if (!d.ac || !d.bc || !fdb || !bdb) rt_fatal("cobalt: out of memory");
    d.fd = fdb + nb + 1, d.bd = bdb + nb + 1;
    compareseq(&d, 0, na, 0, nb);
    int i = 0, j = 0;
    while (i < na || j < nb) {
        if (i < na && j < nb && !d.ac[i] && !d.bc[j]) {
            i++, j++;
            continue;
        }
        int i0 = i, j0 = j;
        while (i < na && d.ac[i]) i++;
        while (j < nb && d.bc[j]) j++;
        if (i == i0 && j == j0) break;   // unreachable with a consistent diff
        if (i0 == i) {   // insert above base token i0 (the end slot when i0 == na)
            for (int k = j - 1; k >= j0; k--) refs_push(&arena[i0].front, &patch->v[k]);
        } else {
            for (int k = i0; k < i; k++) arena[k].deleted = 1;
            if (j > j0) {   // replace: the new tokens go below the first replaced one
                Slot *s = &arena[i0];
                uint32_t sl, ll;
                const char *sb = block_tag(&base->v[i0], &sl);
                for (int k = j - 1; k >= j0; k--) {
                    const Tok *l = &patch->v[k];
                    const char *lb = block_tag(l, &ll);
                    int done = 0;
                    if (sb && lb && sl == ll && !memcmp(sb, lb, sl))
                        for (int q = 0; q < s->back.n && !done; q++) {
                            uint32_t pl;
                            const char *pb = block_tag(s->back.v[q], &pl);
                            if (pb && pl == sl && !memcmp(pb, sb, sl)) s->back.v[q] = l, done = 1;
                        }
                    if (!done) refs_push(&s->back, l);
                }
            }
        }
    }
    free(d.ac), free(d.bc), free(fdb), free(bdb);
}

static int tok_starts(const Tok *t, const char *s) { return t->nn >= strlen(s) && !memcmp(t->np, s, strlen(s)); }

// Writes token t, its Count="..." set to count when count >= 0.
static void emit(Buf *o, const Tok *t, long count) {
    const char *c = count >= 0 ? NULL : t->p;
    if (count >= 0)
        for (const char *p = t->p; p + 7 <= t->p + t->n; p++)
            if (!memcmp(p, "Count=\"", 7)) {
                c = p;
                break;
            }
    if (count < 0 || !c) {
        buf_add(o, t->p, t->n);
        return;
    }
    const char *q = c + 7, *end = t->p + t->n;
    while (q < end && *q != '"') q++;
    char num[24];
    snprintf(num, sizeof num, "%ld", count);
    buf_add(o, t->p, (size_t)(c + 7 - t->p)), buf_str(o, num), buf_add(o, q, (size_t)(end - q));
}

// base (UTF-8, BOM stripped) with every patch applied; the Sheet / Book counts recomputed.
static uint8_t *xml_merge(const char *base, size_t bn, char **patches, size_t *pn, int np, size_t *out_n) {
    Toks b = tokenize(base, bn);
    Slot *arena = calloc((size_t)b.n + 1, sizeof *arena);
    Toks *pt = calloc((size_t)np + 1, sizeof *pt);
    if (!arena || !pt) rt_fatal("cobalt: out of memory");
    arena[b.n].deleted = 1;
    for (int i = 0; i < np; i++) {
        pt[i] = tokenize(patches[i], pn[i]);
        apply_patch(&b, arena, &pt[i]);
    }
    Refs seq = {0};
    for (int i = 0; i <= b.n; i++) {
        for (int k = arena[i].front.n - 1; k >= 0; k--) refs_push(&seq, arena[i].front.v[k]);
        if (!arena[i].deleted) refs_push(&seq, &b.v[i]);
        for (int k = arena[i].back.n - 1; k >= 0; k--) refs_push(&seq, arena[i].back.v[k]);
    }
    long *count = malloc(sizeof *count * ((size_t)seq.n + 1));
    if (!count) rt_fatal("cobalt: out of memory");
    int book = -1, sheet = -1, in_data = 0;
    long rows = 0, sheets = 0;
    for (int i = 0; i < seq.n; i++) {
        const Tok *t = seq.v[i];
        count[i] = -1;
        if (tok_starts(t, "<Book")) book = i, sheets = 0;
        else if (tok_starts(t, "<Sheet")) sheet = i, rows = 0, sheets++;
        else if (t->nn == 6 && !memcmp(t->np, "<Data>", 6)) in_data = 1;
        else if (t->nn == 7 && !memcmp(t->np, "</Data>", 7)) in_data = 0;
        else if (tok_starts(t, "</Sheet") && sheet >= 0) count[sheet] = rows;
        else if (tok_starts(t, "</Book") && book >= 0) count[book] = sheets;
        else if (in_data && tok_starts(t, "<Param")) rows++;
    }
    Buf o = {0};
    buf_add(&o, "\xef\xbb\xbf", 3);
    for (int i = 0; i < seq.n; i++) {
        if (i) buf_add(&o, "\n", 1);
        emit(&o, seq.v[i], count[i]);
    }
    free(count), free(seq.v);
    for (int i = 0; i <= b.n; i++) free(arena[i].front.v), free(arena[i].back.v);
    for (int i = 0; i < np; i++) free(pt[i].v);
    free(arena), free(pt), free(b.v);
    *out_n = o.n;
    return o.p;
}

typedef struct { char stem[256]; char **text; size_t *len; int n; } XmlGroup;
typedef struct { XmlGroup *v; int n; } XmlGroups;
typedef struct { char names[256][256]; int n; } Names;

static int xml_list_cb(const char *name, int kind, uint64_t size, void *ctx) {
    (void)size;
    Names *nm = ctx;
    if (kind == PLAT_FS_FILE && name[0] != '.' && ends_ci(name, ".xml") && nm->n < 256)
        snprintf(nm->names[nm->n++], 256, "%s", name);
    return 1;
}

static void bake_xml(void) {
    XmlGroups g = {calloc(1024, sizeof(XmlGroup)), 0};
    if (!g.v) return;
    for (int i = 0; i < norder; i++) {
        char d[2200];
        mod_sub(order[i], "patches/xml", d, sizeof d);
        if (!plat_dir_exists(d)) continue;
        Names *nm = calloc(1, sizeof *nm);
        if (!nm) continue;
        plat_fs_list(d, xml_list_cb, nm);
        for (int k = 0; k < nm->n; k++) {
            char file[2600], stem[256];
            snprintf(file, sizeof file, "%s/%s", d, nm->names[k]);
            lower_copy(stem, nm->names[k], sizeof stem);
            stem[strlen(stem) - 4] = 0;
            uint64_t n = 0;
            char *t = (char *)plat_file_read(file, &n);
            if (!t) continue;
            int gi = 0;
            while (gi < g.n && strcmp(g.v[gi].stem, stem)) gi++;
            if (gi == g.n) {
                if (g.n == 1024) {
                    free(t);
                    continue;
                }
                snprintf(g.v[g.n++].stem, sizeof g.v->stem, "%s", stem);
            }
            XmlGroup *x = &g.v[gi];
            x->text = realloc(x->text, sizeof *x->text * (size_t)(x->n + 1));
            x->len = realloc(x->len, sizeof *x->len * (size_t)(x->n + 1));
            int bom = n >= 3 && !memcmp(t, "\xef\xbb\xbf", 3);
            memmove(t, t + 3 * bom, n - 3 * bom);
            x->text[x->n] = t, x->len[x->n++] = n - 3 * bom;
        }
        free(nm);
    }
    for (int i = 0; i < g.n; i++) {
        XmlGroup *x = &g.v[i];
        char rom[600];
        snprintf(rom, sizeof rom, BUNDLE_DIR "/fe_assets_gamedata/%s.xml.bundle", x->stem);
        TextBundle tb;
        if (!tb_open(rom, &tb)) {
            rt_log("cobalt: xml: the game has no %s; %d patch(es) left out", rom, x->n);
        } else {
            size_t skip = tb.len >= 3 && !memcmp(tb.data, "\xef\xbb\xbf", 3) ? 3 : 0, n = 0;
            double t0 = (double)plat_now_ns() / 1e9;
            uint8_t *m = xml_merge((const char *)tb.data + skip, tb.len - skip, x->text, x->len, x->n, &n);
            if (tb_write(&tb, rom, m, n))
                rt_log("cobalt: xml: %s: %d patch(es) merged in %.2f s", x->stem, x->n, (double)plat_now_ns() / 1e9 - t0);
            free(m);
            tb_free(&tb);
        }
        for (int k = 0; k < x->n; k++) free(x->text[k]);
        free(x->text), free(x->len);
    }
    free(g.v);
}

// ======== MSBT text
typedef struct { char label[256]; uint8_t *text; size_t len; } Msg;   // text: UTF-16LE with its NUL
typedef struct { char target[512]; Msg *v; int n, cap; } MsgFile;      // target: "<lang>/<region>/<file>" or "<file>"
typedef struct { MsgFile *v; int n, cap; } MsgFiles;

static MsgFile *msg_file(MsgFiles *f, const char *target) {
    for (int i = 0; i < f->n; i++)
        if (!strcmp(f->v[i].target, target)) return &f->v[i];
    if (f->n == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 32;
        f->v = realloc(f->v, sizeof *f->v * (size_t)f->cap);
        if (!f->v) rt_fatal("cobalt: out of memory");
    }
    MsgFile *m = &f->v[f->n++];
    memset(m, 0, sizeof *m);
    snprintf(m->target, sizeof m->target, "%s", target);
    return m;
}

static void msg_set(MsgFile *f, const char *label, uint8_t *text, size_t len) {
    for (int i = 0; i < f->n; i++)
        if (!strcmp(f->v[i].label, label)) {
            free(f->v[i].text);
            f->v[i].text = text, f->v[i].len = len;
            return;
        }
    if (f->n == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 64;
        f->v = realloc(f->v, sizeof *f->v * (size_t)f->cap);
        if (!f->v) rt_fatal("cobalt: out of memory");
    }
    Msg *m = &f->v[f->n++];
    snprintf(m->label, sizeof m->label, "%s", label);
    m->text = text, m->len = len;
}

// UTF-8 -> UTF-16LE with a NUL terminator.
static uint8_t *utf16(const char *s, size_t n, size_t *out_n) {
    uint8_t *o = malloc(n * 4 + 2);
    if (!o) return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n;) {
        uint32_t c = (uint8_t)s[i];
        int extra = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
        c &= extra ? 0x3Fu >> extra : 0x7F;
        i++;
        for (int e = 0; e < extra && i < n; e++, i++) c = c << 6 | ((uint8_t)s[i] & 0x3F);
        if (c >= 0x10000) {
            c -= 0x10000;
            uint32_t hi = 0xD800 + (c >> 10), lo = 0xDC00 + (c & 0x3FF);
            o[k++] = (uint8_t)hi, o[k++] = (uint8_t)(hi >> 8), o[k++] = (uint8_t)lo, o[k++] = (uint8_t)(lo >> 8);
        } else {
            o[k++] = (uint8_t)c, o[k++] = (uint8_t)(c >> 8);
        }
    }
    o[k++] = 0, o[k++] = 0;
    *out_n = k;
    return o;
}

static int label_ok(const char *s, size_t n) {
    if (!n) return 0;
    for (size_t i = 0; i < n; i++)
        if (!(s[i] == '_' || (s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z')))
            return 0;
    return 1;
}

// astra-msbt text: "[LABEL]" lines, each followed by its text up to the next label; edge blank lines dropped.
static void parse_astra(MsgFile *f, char *t, size_t n) {
    if (n >= 3 && !memcmp(t, "\xef\xbb\xbf", 3)) t += 3, n -= 3;
    char label[256] = "";
    Buf text = {0};
    size_t p = 0;
    for (;;) {
        size_t e = p;
        while (e < n && t[e] != '\n') e++;
        size_t le = e;
        if (le > p && t[le - 1] == '\r') le--;
        const char *line = t + p;
        size_t ll = le - p;
        int is_label = ll >= 3 && line[0] == '[' && line[ll - 1] == ']' && label_ok(line + 1, ll - 2);
        if (is_label || e >= n) {
            if (!is_label && e > p) {   // the last line
                if (text.n) buf_add(&text, "\n", 1);
                buf_add(&text, line, ll);
            }
            if (label[0]) {
                size_t a = 0, b = text.n;
                while (a < b && text.p[a] == '\n') a++;
                while (b > a && text.p[b - 1] == '\n') b--;
                size_t un = 0;
                uint8_t *u = utf16((const char *)text.p + a, b - a, &un);
                if (u) msg_set(f, label, u, un);
            }
            text.n = 0;
            if (!is_label) break;
            snprintf(label, sizeof label, "%.*s", (int)(ll - 2), line + 1);
        } else if (label[0]) {
            if (text.n || ll) {
                if (text.n) buf_add(&text, "\n", 1);
                buf_add(&text, line, ll);
            }
        }
        p = e + 1;
        if (e >= n) break;
    }
    free(text.p);
}

typedef struct { MsgFiles *files; } MsgWalk;

static void msg_cb(const char *file, const char *rel, uint64_t size, void *ctx) {
    (void)size;
    MsgWalk *w = ctx;
    if (!ends_ci(rel, ".txt")) return;
    char target[512];
    lower_copy(target, rel, sizeof target);
    target[strlen(target) - 4] = 0;
    uint64_t n = 0;
    char *t = (char *)plat_file_read(file, &n);
    if (!t) return;
    parse_astra(msg_file(w->files, target), t, n);
    free(t);
}

static uint32_t le32u(const uint8_t *p) { return (uint32_t)rd32(p); }

// m (an MSBT with LBL1 / ATR1 / TXT2) with the labels of every file in fs[0..nf) set, later files winning.
static uint8_t *msbt_merge(const uint8_t *m, size_t n, MsgFile **fs, int nf, size_t *out_n) {
    if (n < 32 || memcmp(m, "MsgStdBn", 8)) return NULL;
    const uint8_t *lbl = NULL, *atr = NULL, *txt = NULL;
    uint32_t lbl_n = 0, atr_n = 0, txt_n = 0;
    char order_s[8][4];
    int nsec = 0;
    for (size_t o = 32; o + 16 <= n && nsec < 8;) {
        uint32_t sz = le32u(m + o + 4);
        if (sz > n - o - 16) return NULL;
        memcpy(order_s[nsec++], m + o, 4);
        if (!memcmp(m + o, "LBL1", 4)) lbl = m + o + 16, lbl_n = sz;
        else if (!memcmp(m + o, "ATR1", 4)) atr = m + o + 16, atr_n = sz;
        else if (!memcmp(m + o, "TXT2", 4)) txt = m + o + 16, txt_n = sz;
        o = (o + 16 + sz + 15) & ~(size_t)15;
    }
    if (!lbl || !txt || lbl_n < 4 || txt_n < 4) return NULL;
    uint32_t nb = le32u(lbl), cnt = le32u(txt);
    if ((size_t)nb * 8 + 4 > lbl_n || (size_t)cnt * 4 + 4 > txt_n) return NULL;
    // Labels by index, and texts.
    typedef struct { char name[256]; const uint8_t *t; size_t tn; uint8_t *own; } Row;
    size_t cap = cnt + 4096;
    Row *rows = calloc(cap, sizeof *rows);
    if (!rows) return NULL;
    for (uint32_t b = 0; b < nb; b++) {
        uint32_t c = le32u(lbl + 4 + 8 * b), off = le32u(lbl + 8 + 8 * b);
        for (uint32_t k = 0; k < c; k++) {
            if (off >= lbl_n) break;
            uint32_t ln = lbl[off];
            if (off + 1 + ln + 4 > lbl_n) break;
            uint32_t idx = le32u(lbl + off + 1 + ln);
            if (idx < cnt) snprintf(rows[idx].name, sizeof rows[idx].name, "%.*s", (int)ln, (const char *)lbl + off + 1);
            off += 1 + ln + 4;
        }
    }
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t a = le32u(txt + 4 + 4 * i), e = i + 1 < cnt ? le32u(txt + 8 + 4 * i) : txt_n;
        if (a > e || e > txt_n) a = e = 0;
        rows[i].t = txt + a, rows[i].tn = e - a;
    }
    size_t nrows = cnt;
    for (int f = 0; f < nf; f++)
        for (int k = 0; k < fs[f]->n; k++) {
            const Msg *g = &fs[f]->v[k];
            size_t i = 0;
            while (i < nrows && strcmp(rows[i].name, g->label)) i++;
            if (i == nrows) {
                if (nrows == cap) continue;
                snprintf(rows[nrows++].name, sizeof rows->name, "%s", g->label);
            }
            rows[i].t = g->text, rows[i].tn = g->len;
        }
    // LBL1: the same bucket count, labels hashed as the format does.
    Buf L = {0}, body = {0}, T = {0}, A = {0};
    buf_i32(&L, (int32_t)nb);
    uint32_t off = 4 + 8 * nb;
    for (uint32_t b = 0; b < nb; b++) {
        uint32_t c = 0, start = (uint32_t)body.n;
        for (size_t i = 0; i < nrows; i++) {
            uint32_t h = 0;
            for (const char *p = rows[i].name; *p; p++) h = h * 0x492 + (uint8_t)*p;
            if (h % nb != b) continue;
            uint8_t ln = (uint8_t)strlen(rows[i].name);
            buf_add(&body, &ln, 1), buf_add(&body, rows[i].name, ln), buf_i32(&body, (int32_t)i);
            c++;
        }
        buf_i32(&L, (int32_t)c), buf_i32(&L, (int32_t)(off + start));
    }
    buf_add(&L, body.p, body.n);
    if (atr) {
        uint32_t ac = le32u(atr), as = atr_n >= 8 ? le32u(atr + 4) : 0;
        buf_i32(&A, (int32_t)nrows), buf_i32(&A, (int32_t)as);
        if (as && (size_t)as * ac + 8 <= atr_n) {
            buf_add(&A, atr + 8, (size_t)as * ac);
            uint8_t z = 0;
            for (size_t i = ac; i < nrows; i++)
                for (uint32_t q = 0; q < as; q++) buf_add(&A, &z, 1);
        } else if (atr_n > 8) {
            buf_add(&A, atr + 8, atr_n - 8);
        }
    }
    buf_i32(&T, (int32_t)nrows);
    uint32_t to = 4 + 4 * (uint32_t)nrows;
    for (size_t i = 0; i < nrows; i++) buf_i32(&T, (int32_t)to), to += (uint32_t)rows[i].tn;
    for (size_t i = 0; i < nrows; i++) buf_add(&T, rows[i].t, rows[i].tn);
    Buf o = {0};
    buf_add(&o, m, 32);
    int written = 0;
    for (int s = 0; s < nsec; s++) {
        Buf *d = !memcmp(order_s[s], "LBL1", 4) ? &L : !memcmp(order_s[s], "ATR1", 4) ? &A
               : !memcmp(order_s[s], "TXT2", 4) ? &T : NULL;
        if (!d) continue;
        uint8_t h[16] = {0};
        memcpy(h, order_s[s], 4), wr32(h + 4, (int32_t)d->n);
        buf_add(&o, h, 16), buf_add(&o, d->p ? d->p : (uint8_t *)"", d->n);
        uint8_t pad = 0xAB;
        while (o.n % 16) buf_add(&o, &pad, 1);
        written++;
    }
    o.p[14] = (uint8_t)written, o.p[15] = 0;
    wr32(o.p + 18, (int32_t)o.n);
    free(L.p), free(body.p), free(T.p), free(A.p), free(rows);
    *out_n = o.n;
    return o.p;
}

static void bake_msbt(void) {
    MsgFiles files = {0};
    MsgWalk w = {&files};
    // Each mod in order; inside one, the language-wide files first so its language files win.
    for (int i = 0; i < norder; i++) {
        char d[2200];
        mod_sub(order[i], "patches/msbt/message", d, sizeof d);
        walk(d, "", msg_cb, &w);
    }
    if (!files.n) return;
    // The targets: each language file a mod names, and every language's copy of a language-wide one.
    char (*targets)[512] = calloc(8192, 512);
    int nt = 0;
    if (!targets) return;
    for (int i = 0; i < files.n; i++) {
        const char *t = files.v[i].target;
        if (strchr(t, '/')) {
            int dup = 0;
            for (int k = 0; k < nt && !dup; k++) dup = !strcmp(targets[k], t);
            if (!dup && nt < 8192) snprintf(targets[nt++], 512, "%s", t);
            continue;
        }
        size_t pre = strlen(BUNDLE_DIR "/fe_assets_message/");
        char tail[600];
        snprintf(tail, sizeof tail, "/%s.bytes.bundle", t);
        for (uint64_t r = 0; r < romfs_file_count(); r++) {
            const char *name = romfs_entry(r, NULL, NULL);
            if (strncmp(name, BUNDLE_DIR "/fe_assets_message/", pre) || !ends_ci(name, tail)) continue;
            char tg[512];
            lower_copy(tg, name + pre, sizeof tg);
            tg[strlen(tg) - strlen(".bytes.bundle")] = 0;
            int dup = 0;
            for (int k = 0; k < nt && !dup; k++) dup = !strcmp(targets[k], tg);
            if (!dup && nt < 8192) snprintf(targets[nt++], 512, "%s", tg);
        }
    }
    int done = 0;
    for (int k = 0; k < nt; k++) {
        const char *tg = targets[k], *leaf = strrchr(tg, '/');
        MsgFile *fs[64];
        int nf = 0;
        for (int i = 0; i < files.n && nf < 64; i++)   // language-wide first, then this language's
            if (!strchr(files.v[i].target, '/') && leaf && !strcmp(files.v[i].target, leaf + 1)) fs[nf++] = &files.v[i];
        for (int i = 0; i < files.n && nf < 64; i++)
            if (!strcmp(files.v[i].target, tg)) fs[nf++] = &files.v[i];
        char rom[700];
        snprintf(rom, sizeof rom, BUNDLE_DIR "/fe_assets_message/%s.bytes.bundle", tg);
        if (!romfs_base_exists(rom)) {
            rt_log("cobalt: msbt: the game has no %s; left out", rom);
            continue;
        }
        TextBundle tb;
        if (!tb_open(rom, &tb)) continue;
        size_t n = 0;
        uint8_t *m = msbt_merge(tb.data, tb.len, fs, nf, &n);
        if (!m) rt_log("cobalt: msbt: %s is not an MSBT this merge reads", rom);
        else if (tb_write(&tb, rom, m, n)) done++;
        free(m);
        tb_free(&tb);
    }
    rt_log("cobalt: msbt: %d message files merged", done);
    for (int i = 0; i < files.n; i++) {
        for (int k = 0; k < files.v[i].n; k++) free(files.v[i].v[k].text);
        free(files.v[i].v);
    }
    free(files.v), free(targets);
}

// ======== the cache and the start
static void digest_cb(const char *file, const char *rel, uint64_t size, void *ctx) {
    Sha256 *h = ctx;
    PlatFile *f = plat_file_open(file);
    PlatFileId id = {0};
    if (f) plat_file_id(f, &id), plat_file_close(f);
    char line[64];
    snprintf(line, sizeof line, "\t%llu %llu\n", (unsigned long long)size, (unsigned long long)id.write_time);
    sha256_update(h, rel, strlen(rel)), sha256_update(h, line, strlen(line));
}

static void key_of(char hex[65]) {
    Sha256 h;
    sha256_init(&h);
    sha256_update(&h, BAKE_VERSION, strlen(BAKE_VERSION));
    sha256_update(&h, ROMFS_INDEX_SHA256, strlen(ROMFS_INDEX_SHA256));
    romfs_overlays_digest(&h);   // every mod file served, with its order
    for (int i = 0; i < norder; i++) {
        const ModInfo *m = mods_at(order[i]);
        char d[2200];
        sha256_update(&h, m->name, strlen(m->name) + 1);
        mod_sub(order[i], "patches", d, sizeof d);
        walk(d, "", digest_cb, &h);
    }
    uint8_t md[32];
    sha256_final(&h, md);
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", md[i]);
}

static void register_outputs(void) {
    for (int i = 0; i < noutputs; i++) {
        char path[2600];
        out_path(outputs[i], path, sizeof path);
        if (!romfs_cobalt_overlay(outputs[i], path)) rt_log("cobalt: cannot open %s", path);
    }
}

void cobalt_bake(void) {
    norder = 0;
    for (int k = mods_count() - 1; k >= 0 && norder < MAX_COBALT; k--) {
        const ModInfo *m = mods_at(k);
        if (m->enabled && (m->kind & MOD_COBALT) && m->problem == MOD_OK) order[norder++] = k;
    }
    if (!norder) return;
    snprintf(cache_dir, sizeof cache_dir, "%s/cobalt", rt_config()->profile);
    plat_file_mkdir_p(cache_dir);
    char key[65], manifest[2200];
    key_of(key);
    snprintf(manifest, sizeof manifest, "%s/merged.txt", cache_dir);
    uint64_t n = 0;
    char *t = (char *)plat_file_read(manifest, &n);
    if (t && n >= 65 && !memcmp(t, key, 64) && t[64] == '\n') {
        int ok = 1;
        noutputs = 0;
        for (char *p = t + 65, *end = t + n; p < end && ok;) {
            char *nl = memchr(p, '\n', (size_t)(end - p));
            if (!nl) break;
            *nl = 0;
            char path[2600];
            out_path(p, path, sizeof path);
            if (!plat_file_exists(path)) ok = 0;
            else if (noutputs < (int)(sizeof outputs / sizeof *outputs)) snprintf(outputs[noutputs++], sizeof *outputs, "%s", p);
            p = nl + 1;
        }
        free(t);
        if (ok) {
            rt_log("cobalt: %d mod(s), %d merged file(s) from the cache (%s)", norder, noutputs, cache_dir);
            register_outputs();
            return;
        }
    } else {
        free(t);
    }
    double t0 = (double)plat_now_ns() / 1e9;
    noutputs = 0;
    bake_catalog();
    bake_xml();
    bake_msbt();
    Buf m = {0};
    buf_add(&m, key, 64), buf_add(&m, "\n", 1);
    for (int i = 0; i < noutputs; i++) buf_str(&m, outputs[i]), buf_add(&m, "\n", 1);
    plat_file_write_atomic(manifest, m.p, m.n);
    free(m.p);
    rt_log("cobalt: %d mod(s) merged into %d file(s) in %.1f s (%s)", norder, noutputs,
           (double)plat_now_ns() / 1e9 - t0, cache_dir);
    register_outputs();
}
