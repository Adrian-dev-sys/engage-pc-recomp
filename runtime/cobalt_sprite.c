// Cobalt's custom sprites (the behaviour of Cobalt's sprite.rs): PNG files under a mod's patches/icon and patches/ui
// become Unity sprites through the game's own Texture2D, ImageConversion.LoadImage and Sprite.Create, and are put in
// the game's icon atlases, returned by the face picture getters, or set on the world map's chapter thumbnail.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cobalt_sprite.h"
#include "cobalt_voice.h"
#include "hooks.h"
#include "host.h"
#include "platform/platform.h"

// main offsets (tools/il2cpp_map.py, tools/il2cpp_usages.py)
#define INIT_METADATA 0x492060
#define ARRAY_NEW 0x4920a0            // (class, length)
#define OBJECT_NEW 0x4921f0           // (class)
#define WBARRIER 0x491fb0             // (field address, value), after a reference store into a managed object
#define GOT_BYTE_ARRAY 0x6283ef0      // address of the byte[] class slot
#define GOT_SPRITE_ARRAY 0x6290310    // address of the Sprite[] class slot
#define SLOT_TEXTURE2D 0x615bb20      // class Texture2D
#define SLOT_GAMEICON 0x61579c8       // class GameIcon
#define SLOT_FACETHUMB 0x61575e8      // class FaceThumbnail
#define SLOT_MAPUIGAUGE 0x6159158     // class MapUIGauge
#define SLOT_DICT_ADD 0x6169968       // Dictionary<string,Sprite>.Add
#define SLOT_JOB_LIST 0x617df60       // StructData<JobData>.GetList
#define SLOT_PERSON_LIST 0x617e1d0    // StructData<PersonData>.GetList
#define TEX2D_CTOR 0x378bc10          // Texture2D(int, int)
#define TEX2D_CTOR5 0x378b880         // Texture2D(int, int, GraphicsFormat, int mips, TextureCreationFlags)
#define TEX2D_LOAD_RAW 0x378c590      // LoadRawTextureData(IntPtr, int)
#define TEX2D_APPLY 0x378c8c0         // Apply(bool)
#define TEX2D_PIXELS32 0x378b3d0      // GetPixels32(int mip) -> Color32[] (bottom row first)
#define TEX_SET_FILTER 0x3789790
#define TEX_SET_ANISO 0x37897e0
#define LOAD_IMAGE 0x3f46f20          // ImageConversion.LoadImage(Texture2D, byte[]) static
#define SPRITE_CREATE 0x2f989c0       // Sprite.Create(tex, Rect, Vector2 pivot, float ppu, uint extrude, SpriteMeshType)
#define ATLAS_GET_SPRITE 0x2f98dc0    // SpriteAtlas.GetSprite(string)
#define IMAGE_SET_SPRITE 0x3becff0
#define GRAPHIC_SET_ALL_DIRTY 0x318c790
#define GRAPHIC_SET_VERTS_DIRTY 0x318c9a0
#define RING_GET_ICON 0x2424690
#define GOD_GET_ASCII 0x232ddd0
#define SPOT_GET_CHAPTER 0x2b3ffa0
#define CHAPTER_PREFIXLESS_CID 0x2af9b20
#define TRY_GET_UNIT_ICON 0x227d710   // GameIcon.TyrGetUnitIconIndex(string) static
#define JOB_ICON_F 0x2053dc0
#define JOB_ICON_M 0x2053da0
#define PERSON_ICON 0x1f25d20
#define PERSON_GET_JOB 0x1f29e30

enum { POINT, BILINEAR, TRILINEAR };
enum { K_SKILL, K_ITEM, K_GODRING, K_GODSYMBOL, K_SYSTEM, K_FT_UNIT, K_FT_EMBLEM, K_FT_BOND, K_FACE, K_GODFACE,
       K_MAPSTATUS, K_INFOTHUMB, K_JOB, K_COLOR, NKINDS };
static const struct { const char *dir, *ext; int w, h, filter; } KIND[NKINDS] = {
    {"patches/icon/skill", ".png", 56, 56, TRILINEAR},
    {"patches/icon/item", ".png", 64, 64, TRILINEAR},
    {"patches/icon/emblem/godring", ".png", 74, 74, TRILINEAR},
    {"patches/icon/emblem/godsymbol", ".png", 72, 72, TRILINEAR},
    {"patches/icon/system", ".png", 48, 48, TRILINEAR},
    {"patches/icon/facethumb/unit", ".png", 188, 74, TRILINEAR},
    {"patches/icon/facethumb/emblem", ".png", 188, 74, TRILINEAR},
    {"patches/icon/facethumb/bond", ".png", 188, 74, TRILINEAR},
    {"patches/icon/face", ".png", 300, 300, POINT},
    {"patches/icon/emblem/godface", ".png", 388, 388, POINT},
    {"patches/icon/mapstatus", ".png", 0, 0, BILINEAR},
    {"patches/ui/gmap/infothumb", ".png", 468, 256, TRILINEAR},
    {"patches/icon/job", ".png", 48, 48, POINT},
    {"patches/icon/emblem/godsymbol", ".txt", 0, 0, 0},
};

typedef struct { int kind; char name[128]; char *path; } File;
static File *files;
static int nfiles, capfiles, nkind[NKINDS];

#define RD(a) (*(const uint64_t *)(uintptr_t)(a))
#define RD32(a) (*(const int32_t *)(uintptr_t)(a))
#define G(off) hook_main(off)

static int find(int kind, const char *name) {
    for (int i = 0; i < nfiles; i++)
        if (files[i].kind == kind && !strcmp(files[i].name, name)) return i;
    return -1;
}

typedef struct { int kind; const char *dir; } ListCtx;

static int list_cb(const char *name, void *ctx) {
    const ListCtx *l = ctx;
    size_t n = strlen(name);
    if (n < 5 || n - 4 >= sizeof files->name || name[0] == '.') return 1;
    char stem[128], path[2600];
    snprintf(stem, sizeof stem, "%.*s", (int)(n - 4), name);
    snprintf(path, sizeof path, "%s/%s", l->dir, name);
    int i = find(l->kind, stem);   // a later (higher) mod replaces the file
    if (i < 0) {
        if (nfiles == capfiles) {
            capfiles = capfiles ? capfiles * 2 : 64;
            File *f = realloc(files, sizeof *files * (size_t)capfiles);
            if (!f) return 0;
            files = f;
        }
        i = nfiles++;
        files[i].kind = l->kind;
        snprintf(files[i].name, sizeof files->name, "%s", stem);
        nkind[l->kind]++;
    } else {
        free(files[i].path);
    }
    files[i].path = strdup(path);
    return 1;
}

void cobalt_sprite_add_mod(const char *root) {
    for (int k = 0; k < NKINDS; k++) {
        char dir[2200];
        snprintf(dir, sizeof dir, "%s/%s", root, KIND[k].dir);
        if (!plat_dir_exists(dir)) continue;
        ListCtx l = {k, dir};
        plat_file_list(dir, KIND[k].ext, list_cb, &l);
    }
}

// ---- game helpers

static void utf8(uint64_t s, char *out, size_t cap) {   // a game string as UTF-8 (BMP is enough for names)
    out[0] = 0;
    if (!s) return;
    int32_t len = RD32(s + 0x10);
    const uint16_t *w = (const uint16_t *)(uintptr_t)(s + 0x14);
    size_t k = 0;
    for (int32_t q = 0; q < len && k + 4 < cap; q++) {
        uint32_t ch = w[q];
        if (ch < 0x80) {
            out[k++] = (char)ch;
        } else if (ch < 0x800) {
            out[k++] = (char)(0xC0 | ch >> 6), out[k++] = (char)(0x80 | (ch & 0x3F));
        } else {
            out[k++] = (char)(0xE0 | ch >> 12), out[k++] = (char)(0x80 | (ch >> 6 & 0x3F));
            out[k++] = (char)(0x80 | (ch & 0x3F));
        }
    }
    out[k] = 0;
}

static uint64_t slot(Cpu *c, uint64_t addr) {   // an initialised metadata slot's value
    guest_call(c, G(INIT_METADATA), addr, 0, 0, 0);
    return RD(addr);
}

static void store(Cpu *c, uint64_t field, uint64_t value) {
    *(uint64_t *)(uintptr_t)field = value;
    guest_call(c, G(WBARRIER), field, value, 0, 0);
}

static uint64_t statics(Cpu *c, uint64_t class_slot) {   // Il2CppClass.static_fields
    uint64_t k = slot(c, G(class_slot));
    return k ? RD(k + 0xb8) : 0;
}

static void set_f(Cpu *c, int i, float f) {
    uint32_t b;
    memcpy(&b, &f, 4);
    c->v[i] = b;
}

static uint64_t g_string;   // any game string, for the string class

// Slots on the guest stack for objects only host code holds: the GC scans guest stacks, not ours, and could free them
// at the next allocation. Restore c->sp with the returned value.
static uint64_t roots(Cpu *c, int n, uint64_t **slots) {
    uint64_t sp = c->sp;
    c->sp = (sp - 8 * (uint64_t)n) & ~15ull;
    *slots = (uint64_t *)(uintptr_t)c->sp;
    memset(*slots, 0, 8 * (size_t)n);
    return sp;
}

static uint64_t str(const char *s) { return cobalt_string(g_string, s); }

// A sprite from a PNG file through the game's own decoder. w, h: the size Cobalt requires (0: any).
static uint64_t make_sprite_(Cpu *c, const File *f, int w, int h, int filter, uint64_t *tex_out, uint64_t *r);

static uint64_t make_sprite(Cpu *c, const File *f, int w, int h, int filter, uint64_t *tex_out) {
    uint64_t *r, sp = roots(c, 2, &r);
    uint64_t s = make_sprite_(c, f, w, h, filter, tex_out, r);
    c->sp = sp;
    return s;
}

static uint64_t make_sprite_(Cpu *c, const File *f, int w, int h, int filter, uint64_t *tex_out, uint64_t *r) {
    uint64_t n = 0;
    uint8_t *png = (uint8_t *)plat_file_read(f->path, &n);
    if (!png) return 0;
    if (n < 24 || memcmp(png, "\x89PNG", 4)) {
        rt_log("cobalt: sprite: %s is not a PNG file", f->path);
        free(png);
        return 0;
    }
    int pw = png[16] << 24 | png[17] << 16 | png[18] << 8 | png[19];
    int ph = png[20] << 24 | png[21] << 16 | png[22] << 8 | png[23];
    if (w && (pw != w || ph != h)) {
        rt_log("cobalt: sprite: %s is %dx%d, expected %dx%d; skipped", f->path, pw, ph, w, h);
        free(png);
        return 0;
    }
    uint64_t arr = guest_call(c, G(ARRAY_NEW), slot(c, RD(G(GOT_BYTE_ARRAY))), n, 0, 0);
    if (!arr) {
        free(png);
        return 0;
    }
    r[0] = arr;
    memcpy((void *)(uintptr_t)(arr + 0x20), png, n);
    free(png);
    uint64_t tex = guest_call(c, G(OBJECT_NEW), slot(c, G(SLOT_TEXTURE2D)), 0, 0, 0);
    if (!tex) return 0;
    r[1] = tex;
    guest_call(c, G(TEX2D_CTOR), tex, (uint32_t)pw, (uint32_t)ph, 0);
    if (!(guest_call(c, G(LOAD_IMAGE), tex, arr, 0, 0) & 1)) {
        rt_log("cobalt: sprite: the game could not decode %s", f->path);
        return 0;
    }
    guest_call(c, G(TEX_SET_FILTER), tex, (uint32_t)filter, 0, 0);
    if (tex_out) *tex_out = tex;
    set_f(c, 0, 0), set_f(c, 1, 0), set_f(c, 2, (float)pw), set_f(c, 3, (float)ph);   // Rect
    set_f(c, 4, 0.5f), set_f(c, 5, 0.5f), set_f(c, 6, 100.f);                        // pivot, pixels per unit
    return guest_call(c, G(SPRITE_CREATE), tex, 1, 1 /* Tight */, 0);
}

// Dictionary<string,Sprite>: replace the value of key, or add it.
static void dict_put(Cpu *c, uint64_t dict, const char *key, uint64_t value, int *added) {
    uint64_t entries = RD(dict + 0x18);
    int32_t count = RD32(dict + 0x20);
    for (int32_t i = 0; entries && i < count && i < RD32(entries + 0x18); i++) {
        uint64_t e = entries + 0x20 + 24 * (uint64_t)i;   // {int hash, int next, string key, Sprite value}
        char k[160];
        if (RD32(e) < 0) continue;
        utf8(RD(e + 8), k, sizeof k);
        if (!strcmp(k, key)) {
            store(c, e + 16, value);
            return;
        }
    }
    uint64_t mi = slot(c, G(SLOT_DICT_ADD)), *r, sp = roots(c, 1, &r);
    r[0] = value;
    guest_call(c, RD(mi), dict, str(key), value, mi);
    c->sp = sp;
    if (added) (*added)++;
}

static uint64_t dict_get(uint64_t dict, const char *key) {
    uint64_t entries = RD(dict + 0x18);
    int32_t count = RD32(dict + 0x20);
    for (int32_t i = 0; entries && i < count && i < RD32(entries + 0x18); i++) {
        uint64_t e = entries + 0x20 + 24 * (uint64_t)i;
        char k[160];
        if (RD32(e) < 0) continue;
        utf8(RD(e + 8), k, sizeof k);
        if (!strcmp(k, key)) return RD(e + 16);
    }
    return 0;
}

static void keep_string(uint64_t dict) {   // a string from a dictionary's keys, for the string class
    uint64_t entries = dict ? RD(dict + 0x18) : 0;
    for (int32_t i = 0; !g_string && entries && i < RD32(dict + 0x20); i++)
        if (RD32(entries + 0x20 + 24 * (uint64_t)i) >= 0) g_string = RD(entries + 0x20 + 24 * (uint64_t)i + 8);
}

// ---- atlases (MainSequence.PostLoadResource)

static void inject(Cpu *c, uint64_t atlas_manager, int kind, int *added, int *changed) {
    if (!atlas_manager || !nkind[kind]) return;
    uint64_t dict = RD(atlas_manager + 0x20);   // SpriteAtlasManager.m_CacheTable
    if (!dict) return;
    keep_string(dict);
    for (int i = 0; i < nfiles; i++) {
        if (files[i].kind != kind) continue;
        uint64_t s = make_sprite(c, &files[i], KIND[kind].w, KIND[kind].h, KIND[kind].filter, NULL);
        if (!s) continue;
        int a = 0;
        dict_put(c, dict, files[i].name, s, &a);
        *added += a, *changed += !a;
    }
}

static void add_default_unit_sprites(Cpu *c);

static int post_load_resource(Cpu *c) {
    hook_original(c, "cobalt_post_load_resource", c->x[0], c->x[1], 0, 0);
    int added = 0, changed = 0;
    uint64_t gi = statics(c, SLOT_GAMEICON), ft = statics(c, SLOT_FACETHUMB);
    if (ft)
        for (int k = K_FT_UNIT; k <= K_FT_BOND; k++) inject(c, RD(ft + 0x8), k, &added, &changed);
    if (gi) {
        inject(c, RD(gi + 0x0), K_SKILL, &added, &changed);
        inject(c, RD(gi + 0x8), K_ITEM, &added, &changed);
        inject(c, RD(gi + 0x38), K_GODRING, &added, &changed);
        inject(c, RD(gi + 0x30), K_GODSYMBOL, &added, &changed);
        inject(c, RD(gi + 0x40), K_SYSTEM, &added, &changed);
    }
    add_default_unit_sprites(c);
    if (added + changed) rt_log("cobalt: sprite: %d icon(s) added, %d replaced", added, changed);
    return 1;
}

// ---- pictures loaded on demand

static int sprite_by_name(Cpu *c, int kind, uint64_t getter) {
    char name[128];
    utf8(guest_call(c, G(getter), c->x[0], 0, 0, 0), name, sizeof name);
    int i = name[0] ? find(kind, name) : -1;
    if (i < 0) return 0;
    uint64_t s = make_sprite(c, &files[i], KIND[kind].w, KIND[kind].h, KIND[kind].filter, NULL);
    if (!s) return 0;
    c->x[0] = s;
    return 1;
}

static int ring_face(Cpu *c) { return sprite_by_name(c, K_FACE, RING_GET_ICON); }
static int god_face(Cpu *c) { return sprite_by_name(c, K_GODFACE, GOD_GET_ASCII); }

// GodColorRefineEmblem.GetColor(this, god) -> Color: patches/icon/emblem/godsymbol/<ascii>.txt holds RRGGBB[AA].
static int god_color(Cpu *c) {
    char name[128];
    utf8(guest_call(c, G(GOD_GET_ASCII), c->x[1], 0, 0, 0), name, sizeof name);
    int i = name[0] ? find(K_COLOR, name) : -1;
    if (i < 0) return 0;
    uint64_t n = 0;
    char *t = (char *)plat_file_read(files[i].path, &n), hex[16] = "";
    if (t) {
        const char *p = t;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == '#' || *p == (char)0xEF ||
               *p == (char)0xBB || *p == (char)0xBF)
            p++;
        size_t k = 0;
        while (k < 9 && ((p[k] >= '0' && p[k] <= '9') || ((p[k] | 32) >= 'a' && (p[k] | 32) <= 'f'))) k++;
        if (k == 6 || k == 8) snprintf(hex, sizeof hex, "%.*s", (int)k, p);
        free(t);
    }
    uint32_t v = (uint32_t)strtoul(hex, NULL, 16);
    float r = 1, g = 1, b = 1, a = 1;   // Cobalt: white for anything else
    if (strlen(hex) == 6) r = (v >> 16 & 255) / 255.f, g = (v >> 8 & 255) / 255.f, b = (v & 255) / 255.f;
    if (strlen(hex) == 8)
        r = (v >> 24) / 255.f, g = (v >> 16 & 255) / 255.f, b = (v >> 8 & 255) / 255.f, a = (v & 255) / 255.f;
    set_f(c, 0, r), set_f(c, 1, g), set_f(c, 2, b), set_f(c, 3, a);
    return 1;
}

// GmapMapInfoContent.SetMapInfo(this, spot): the chapter's thumbnail from patches/ui/gmap/infothumb/<cid>.png
static int map_info(Cpu *c) {
    uint64_t self = c->x[0], spot = c->x[1];
    hook_original(c, "cobalt_gmap_info", self, spot, c->x[2], 0);
    uint64_t chapter = spot ? guest_call(c, G(SPOT_GET_CHAPTER), spot, 0, 0, 0) : 0;
    char cid[128];
    utf8(chapter ? guest_call(c, G(CHAPTER_PREFIXLESS_CID), chapter, 0, 0, 0) : 0, cid, sizeof cid);
    int i = cid[0] ? find(K_INFOTHUMB, cid) : -1;
    uint64_t s = i >= 0 ? make_sprite(c, &files[i], 468, 256, TRILINEAR, NULL) : 0;
    if (s) {
        store(c, self + 0xc0, s);   // m_MapInfoSprite
        if (RD(self + 0x28)) guest_call(c, G(IMAGE_SET_SPRITE), RD(self + 0x28), s, 0, 0);   // m_MapInfoImage
    }
    return 1;
}

// ---- map status icons (MapUIGauge: the game has 0xA7 names in IconNames and a Sprite[0xA7])

#define GAUGE_ICONS 0xa7

static int gauge_cctor(Cpu *c) {
    hook_original(c, "cobalt_gauge_cctor", c->x[0], 0, 0, 0);
    uint64_t st = statics(c, SLOT_MAPUIGAUGE), names = st ? RD(st) : 0;
    if (!names) return 1;
    int32_t n = RD32(names + 0x18), extra = 0;
    g_string = g_string ? g_string : RD(names + 0x20);
    for (int i = 0; i < nfiles; i++) {
        if (files[i].kind != K_MAPSTATUS) continue;
        int have = 0;
        for (int32_t q = 0; q < n && !have; q++) {
            char k[160];
            utf8(RD(names + 0x20 + 8 * (uint64_t)q), k, sizeof k);
            have = !strcmp(k, files[i].name);
        }
        files[i].kind = have ? -1 - K_MAPSTATUS : K_MAPSTATUS;   // a game name stays the atlas's
        extra += !have;
    }
    if (!extra) return 1;
    uint64_t arr = guest_call(c, G(ARRAY_NEW), RD(names), (uint64_t)(n + extra), 0, 0);   // string[]
    if (!arr) return 1;
    uint64_t *r, sp = roots(c, 1, &r);
    r[0] = arr;
    for (int32_t q = 0; q < n; q++) store(c, arr + 0x20 + 8 * (uint64_t)q, RD(names + 0x20 + 8 * (uint64_t)q));
    int32_t q = n;
    for (int i = 0; i < nfiles; i++)
        if (files[i].kind == K_MAPSTATUS) store(c, arr + 0x20 + 8 * (uint64_t)q++, str(files[i].name));
    store(c, st, arr);
    c->sp = sp;
    rt_log("cobalt: sprite: %d map status icon(s) added", extra);
    return 1;
}

static uint64_t empty_sprite(Cpu *c) {   // a Sprite object without an asset, as Cobalt returns for a missing icon
    uint64_t arr_class = slot(c, RD(G(GOT_SPRITE_ARRAY)));
    return arr_class ? guest_call(c, G(OBJECT_NEW), RD(arr_class + 0x40), 0, 0, 0) : 0;   // element_class
}

static int gauge_on_enable(Cpu *c) {
    uint64_t self = c->x[0];
    if (RD(self + 0x58)) return 1;   // m_Sprites
    uint64_t st = statics(c, SLOT_MAPUIGAUGE), names = st ? RD(st) : 0;
    if (!names || RD32(names + 0x18) <= GAUGE_ICONS) return 0;   // no added names: the game's own code
    int32_t n = RD32(names + 0x18);
    uint64_t atlas = RD(self + 0x48), dict = RD(self + 0x60);
    uint64_t arr = guest_call(c, G(ARRAY_NEW), slot(c, RD(G(GOT_SPRITE_ARRAY))), (uint64_t)n, 0, 0);
    if (!arr || !dict) return 0;
    uint64_t *r, sp = roots(c, 1, &r);
    r[0] = arr;
    for (int32_t q = 0; q < n; q++) {
        uint64_t name = RD(names + 0x20 + 8 * (uint64_t)q), s = 0;
        char k[160];
        utf8(name, k, sizeof k);
        if (q < GAUGE_ICONS) {
            s = atlas ? guest_call(c, G(ATLAS_GET_SPRITE), atlas, name, 0, 0) : 0;
        } else {
            int i = find(K_MAPSTATUS, k);
            s = i >= 0 ? make_sprite(c, &files[i], 0, 0, BILINEAR, NULL) : 0;
            if (!s) s = empty_sprite(c);
        }
        store(c, arr + 0x20 + 8 * (uint64_t)q, s);
        dict_put(c, dict, k, s, NULL);
    }
    store(c, self + 0x58, arr);
    c->sp = sp;
    return 1;
}

static int gauge_get_sprite(Cpu *c) {   // GetSprite(int): any index of the longer array
    uint64_t arr = RD(c->x[0] + 0x58);
    int32_t i = (int32_t)c->x[1];
    uint64_t s = arr && i >= 0 && i < RD32(arr + 0x18) ? RD(arr + 0x20 + 8 * (uint64_t)i) : 0;
    c->x[0] = s ? s : empty_sprite(c);
    return 1;
}

// ---- unit icons (patches/icon/job/<Unit>_<Job>.png, 48x48): a palette and an index texture, as the game's shader
// expects

static int find_job_icon(const char *index) {
    int i = find(K_JOB, index);
    if (i >= 0) return i;
    char unit[128], fb[260];
    snprintf(unit, sizeof unit, "%s", index);
    char *u = strchr(unit, '_');
    if (u) *u = 0;
    snprintf(fb, sizeof fb, "%s_%s", unit, unit);   // Unit_Unit.png when Unit_Job.png is missing
    return find(K_JOB, fb);
}

static uint64_t raw_sprite(Cpu *c, int w, int h, int format, const void *data, int size, int full_rect, float pivot) {
    uint64_t tex = guest_call(c, G(OBJECT_NEW), slot(c, G(SLOT_TEXTURE2D)), 0, 0, 0);
    if (!tex) return 0;
    uint64_t *r, sp = roots(c, 1, &r);
    r[0] = tex;
    guest_call6(c, G(TEX2D_CTOR5), tex, (uint32_t)w, (uint32_t)h, (uint32_t)format, 1, 0);
    guest_call(c, G(TEX_SET_FILTER), tex, POINT, 0, 0);
    guest_call(c, G(TEX_SET_ANISO), tex, 1, 0, 0);
    guest_call(c, G(TEX2D_LOAD_RAW), tex, (uint64_t)(uintptr_t)data, (uint32_t)size, 0);
    guest_call(c, G(TEX2D_APPLY), tex, 0, 0, 0);
    set_f(c, 0, 0), set_f(c, 1, 0), set_f(c, 2, (float)w), set_f(c, 3, (float)h);
    set_f(c, 4, pivot), set_f(c, 5, pivot), set_f(c, 6, 100.f);
    uint64_t s = guest_call(c, G(SPRITE_CREATE), tex, 1, full_rect ? 0 : 1, 0);
    c->sp = sp;
    return s;
}

// The palette (512x1, each colour twice, 0 transparent) and index (R8) sprites of a 48x48 icon.
static int unit_sprites_(Cpu *c, int file, uint64_t *palette, uint64_t *index, uint64_t *r);

static int unit_sprites(Cpu *c, int file, uint64_t *palette, uint64_t *index) {
    uint64_t *r, sp = roots(c, 4, &r);
    int ok = unit_sprites_(c, file, palette, index, r);
    c->sp = sp;
    return ok;
}

static int unit_sprites_(Cpu *c, int file, uint64_t *palette, uint64_t *index, uint64_t *r) {
    uint64_t tex = 0;
    if (!(r[0] = make_sprite(c, &files[file], 48, 48, POINT, &tex)) || !tex) return 0;
    r[1] = tex;
    uint64_t px = guest_call(c, G(TEX2D_PIXELS32), tex, 0, 0, 0);
    r[2] = px;   // bottom row first, as the index texture wants
    if (!px || RD32(px + 0x18) != 48 * 48) return 0;
    const uint8_t *rgba = (const uint8_t *)(uintptr_t)(px + 0x20);
    static uint8_t pal[512 * 4];
    static uint8_t idx[48 * 48];
    uint32_t colors[256];
    int ncolors = 1;
    memset(pal, 0, sizeof pal);
    colors[0] = 0;
    for (int p = 0; p < 48 * 48; p++) {
        uint32_t col;
        memcpy(&col, rgba + 4 * p, 4);
        int k = 0;
        while (k < ncolors && colors[k] != col) k++;
        if (k == ncolors) {
            if (ncolors == 256) {
                rt_log("cobalt: sprite: %s has more than 255 colours; skipped", files[file].path);
                return 0;
            }
            colors[ncolors++] = col;
            memcpy(pal + 8 * k, &col, 4), memcpy(pal + 8 * k + 4, &col, 4);
        }
        idx[p] = (uint8_t)k;
    }
    *palette = r[3] = raw_sprite(c, 512, 1, 4 /* R8G8B8A8_SRGB */, pal, sizeof pal, 1, 0.f);
    *index = raw_sprite(c, 48, 48, 5 /* R8_UNorm */, idx, sizeof idx, 0, 0.5f);
    return *palette && *index;
}

static void set_unit_icon(Cpu *c, uint64_t self, uint64_t palette, uint64_t index, int all_dirty) {
    store(c, self + 0x118, palette);   // m_PalleteSprite
    guest_call(c, G(IMAGE_SET_SPRITE), self, index, 0, 0);
    guest_call(c, G(all_dirty ? GRAPHIC_SET_ALL_DIRTY : GRAPHIC_SET_VERTS_DIRTY), self, 0, 0, 0);
}

static int unit_update_icon(Cpu *c) {   // UnitIcon.UpdateIcon(this)
    char name[160];
    utf8(RD(c->x[0] + 0x120), name, sizeof name);   // m_IconName
    int i = name[0] && nkind[K_JOB] ? find_job_icon(name) : -1;
    uint64_t pal, idx;
    if (i < 0 || !unit_sprites(c, i, &pal, &idx)) return 0;
    set_unit_icon(c, c->x[0], pal, idx, 1);
    return 1;
}

static int unit_try_set(Cpu *c) {   // UnitIcon.TrySet(this, index, palette) -> bool
    uint64_t self = c->x[0], index = c->x[1], palette = c->x[2], mi = c->x[3];
    if (index) {
        char name[160];
        utf8(index, name, sizeof name);
        int i = nkind[K_JOB] ? find_job_icon(name) : -1;
        uint64_t pal, idx;
        if (i >= 0 && unit_sprites(c, i, &pal, &idx)) {
            store(c, self + 0x120, index);
            set_unit_icon(c, self, pal, idx, 0);
            c->x[0] = 1;
            return 1;
        }
        if (guest_call(c, G(TRY_GET_UNIT_ICON), index, 0, 0, 0)) return 0;
    }
    if (!palette) return 0;
    char p[128], fb[260];
    utf8(palette, p, sizeof p);
    snprintf(fb, sizeof fb, "%s_%s", p, p);   // Cobalt: <Palette>_<Palette>, which add_default_unit_sprites fills
    g_string = g_string ? g_string : palette;
    c->x[0] = hook_original(c, "cobalt_unit_try_set", self, str(fb), palette, mi);
    return 1;
}

// Every palette gets a <palette>_<palette> unit icon, so a mod's new character falls back to one (Cobalt's
// add_default_unit_sprites).
static void add_default_unit_sprites(Cpu *c) {
    uint64_t gi = statics(c, SLOT_GAMEICON);
    uint64_t im = gi ? RD(gi + 0x48) : 0, pm = gi ? RD(gi + 0x50) : 0;
    uint64_t icons = im ? RD(im + 0x20) : 0, pals = pm ? RD(pm + 0x20) : 0;
    if (!icons || !pals || !RD(icons + 0x18) || !RD(pals + 0x18)) return;
    keep_string(icons);
    int added = 0;
    uint64_t pe = RD(pals + 0x18);
    int32_t npal = RD32(pals + 0x20);
    char key[300], k2[300];
    for (int32_t p = 0; p < npal; p++) {   // <P>_<P>_NoWeapon
        if (RD32(pe + 0x20 + 24 * (uint64_t)p) < 0) continue;
        char pal[128];
        utf8(RD(pe + 0x20 + 24 * (uint64_t)p + 8), pal, sizeof pal);
        snprintf(key, sizeof key, "%s_%s", pal, pal);
        if (dict_get(icons, key)) continue;
        snprintf(k2, sizeof k2, "%s_NoWeapon", key);
        uint64_t s = dict_get(icons, k2);
        if (s) dict_put(c, icons, key, s, &added);
    }
    // Then the first icon of a person with that palette whose job's icon id is in the icon's name.
    uint64_t lmi = slot(c, G(SLOT_PERSON_LIST));
    uint64_t list = lmi ? guest_call(c, RD(lmi), lmi, 0, 0, 0) : 0;   // static GetList(MethodInfo)
    uint64_t items = list ? RD(list + 0x10) : 0;
    int32_t np = list ? RD32(list + 0x18) : 0;
    for (int32_t q = 0; items && q < np; q++) {
        uint64_t person = RD(items + 0x20 + 8 * (uint64_t)q);
        if (!person) continue;
        char pal[128];
        utf8(guest_call(c, G(PERSON_ICON), person, 0, 0, 0), pal, sizeof pal);
        if (!pal[0]) continue;
        snprintf(key, sizeof key, "%s_%s", pal, pal);
        if (dict_get(icons, key) || !dict_get(pals, pal)) continue;
        uint64_t job = guest_call(c, G(PERSON_GET_JOB), person, 0, 0, 0);
        if (!job) continue;
        for (int g = 0; g < 2; g++) {
            char jid[128];
            utf8(guest_call(c, G(g ? JOB_ICON_M : JOB_ICON_F), job, 0, 0, 0), jid, sizeof jid);
            if (!jid[0]) continue;
            uint64_t e = RD(icons + 0x18), s = 0;
            for (int32_t i = 0; !s && i < RD32(icons + 0x20); i++) {
                if (RD32(e + 0x20 + 24 * (uint64_t)i) < 0) continue;
                char k[200];
                utf8(RD(e + 0x20 + 24 * (uint64_t)i + 8), k, sizeof k);
                if (strstr(k, jid) && strstr(k, pal)) s = RD(e + 0x20 + 24 * (uint64_t)i + 16);
            }
            if (s) {
                dict_put(c, icons, key, s, &added);
                break;
            }
        }
    }
    static const char *const fixed[] = {"102Louis_630LanceArmor_Lance", "153Chloe_646LancePegasus_Lance",
                                        "203Umber_637LanceKnight_Lance", "250Jade_631AxArmor_Ax"};
    for (size_t f = 0; f < sizeof fixed / sizeof *fixed; f++) {   // default icons that are not NoWeapon
        char pal[64];
        snprintf(pal, sizeof pal, "%.*s", (int)(strchr(fixed[f], '_') - fixed[f]), fixed[f]);
        snprintf(key, sizeof key, "%s_%s_NoWeapon", pal, pal);
        uint64_t s = dict_get(icons, fixed[f]);
        if (s && !dict_get(icons, key)) dict_put(c, icons, key, s, &added);
    }
    static int logged;
    if (added && !logged++) rt_log("cobalt: sprite: %d default unit icon(s) added", added);
}

void cobalt_sprite_init(void) {
    int n = 0;
    for (int k = 0; k < NKINDS; k++) n += nkind[k];
    if (n) rt_log("cobalt: sprite: %d file(s)", n);
    hook_install("cobalt_post_load_resource", post_load_resource);
    hook_install("cobalt_unit_try_set", unit_try_set);
    if (nkind[K_JOB]) hook_install("cobalt_unit_update_icon", unit_update_icon);
    if (nkind[K_FACE]) hook_install("cobalt_ring_face", ring_face);
    if (nkind[K_GODFACE]) hook_install("cobalt_god_face", god_face);
    if (nkind[K_COLOR]) hook_install("cobalt_god_color", god_color);
    if (nkind[K_INFOTHUMB]) hook_install("cobalt_gmap_info", map_info);
    if (nkind[K_MAPSTATUS]) {
        hook_install("cobalt_gauge_cctor", gauge_cctor);
        hook_install("cobalt_gauge_on_enable", gauge_on_enable);
        hook_install("cobalt_gauge_get_sprite", gauge_get_sprite);
    }
}
