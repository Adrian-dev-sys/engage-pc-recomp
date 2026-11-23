// Fake online (F1 > Game, TOTK_FAKE_ONLINE, off by default): local stand-in data for Engage's online features, no server.
// For now the chapter-map bonus spots (MapKillBonus): glowing tiles where "other players" scored kills or lost units.
#include <stdint.h>
#include <time.h>

#include "cfg.h"
#include "hooks.h"
#include "host.h"

// main offsets (_local/il2cpp dump, tools/il2cpp_usages.py)
#define INIT_METADATA 0x492060
#define SINGLETON_GET 0x1c3b70          // SingletonClass<T>.get_Instance(method)
#define MAPIMAGE_METHOD 0x617b4e8       // metadata slot of SingletonClass<MapImage>.get_Instance's MethodInfo
#define PROC_DOWNLOAD_BIND 0x1e3a5f0    // MapSequenceNet.ProcDownload.CreateBind(super)
#define TERRAIN_GET_DATA 0x2064ed0      // MapImageTerrain.GetData(x, z) -> TerrainData
#define MAPIMAGE_TERRAIN 0x30           // MapImage.m_Terrain
#define MAPIMAGE_PLAY_AREA 0x70         // MapImage.PlayAreaX, Z, W, H (int32 each)
#define TERRAIN_PROHIBITION 0x40        // TerrainData.Prohibition (0 = any unit may stand there)
#define NKB_DATA 0x20                   // NexKillBonus.m_Data: int[2,32,32] (Kill, Killed) per x, z
#define NKB_SUCCEEDED 0x28              // NexKillBonus.m_IsSucceeded
#define ARRAY_DATA 0x20                 // il2cpp array elements

#define RD(a) (*(const uint64_t *)(uintptr_t)(a))
#define G(off) hook_main(off)

static unsigned rng(void) {
    static unsigned s;
    if (!s) s = (unsigned)time(NULL) | 1;
    s = s * 1103515245u + 12345u;
    return s >> 8;
}

// MapSequenceNet.Download(super, isResume) static: the game binds the download proc only with its online byte set.
// Here it is bound without that byte, so rankings and the clear-time upload stay offline.
static int net_download(Cpu *c) {
    if (!cfg_int("TOTK_FAKE_ONLINE")) return 0;
    if (!(c->x[1] & 1)) guest_call(c, G(PROC_DOWNLOAD_BIND), c->x[0], 0, 0, 0);
    return 1;
}

// NexKillBonus.Download(this, super, cid): random counts on walkable cells of the map's play area, reported as
// downloaded at once. MapKillBonus.Setup then picks its own top cells (DecideCells) and places the spots.
static int kill_bonus_download(Cpu *c) {
    if (!cfg_int("TOTK_FAKE_ONLINE")) return 0;
    uint64_t self = c->x[0], arr = RD(self + NKB_DATA);
    c->x[0] = 1;
    *(uint8_t *)(uintptr_t)(self + NKB_SUCCEEDED) = 1;
    if (!arr) return 1;
    int32_t *data = (int32_t *)(uintptr_t)(arr + ARRAY_DATA);
    for (int i = 0; i < 2048; i++) data[i] = 0;

    guest_call(c, G(INIT_METADATA), G(MAPIMAGE_METHOD), 0, 0, 0);
    uint64_t m = RD(G(MAPIMAGE_METHOD));
    uint64_t mi = m ? guest_call(c, G(SINGLETON_GET), m, 0, 0, 0) : 0;
    uint64_t terrain = mi ? RD(mi + MAPIMAGE_TERRAIN) : 0;
    if (!terrain) { rt_log("online: no map image, no bonus spots"); return 1; }
    const int32_t *pa = (const int32_t *)(uintptr_t)(mi + MAPIMAGE_PLAY_AREA);
    int x0 = pa[0], z0 = pa[1], x1 = pa[0] + pa[2], z1 = pa[1] + pa[3];
    if (x0 < 0) x0 = 0;
    if (z0 < 0) z0 = 0;
    if (x1 > 32) x1 = 32;
    if (z1 > 32) z1 = 32;

    uint16_t cells[1024];
    int n = 0;
    for (int x = x0; x < x1; x++)
        for (int z = z0; z < z1; z++) {
            uint64_t td = guest_call(c, G(TERRAIN_GET_DATA), terrain, (uint64_t)x, (uint64_t)z, 0);
            if (td && *(const int32_t *)(uintptr_t)(td + TERRAIN_PROHIBITION) == 0) cells[n++] = (uint16_t)(x << 5 | z);
        }
    // shuffle, then the first cells get Kill counts and the next ones Killed counts (disjoint; the game keeps its top N)
    for (int i = n - 1; i > 0; i--) {
        int j = (int)(rng() % (unsigned)(i + 1));
        uint16_t t = cells[i]; cells[i] = cells[j]; cells[j] = t;
    }
    int per = n / 6 < 24 ? n / 6 : 24;
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < per; i++) {
            int cell = cells[k * per + i];
            data[k * 1024 + (cell >> 5) * 32 + (cell & 31)] = 1 + (int)(rng() % 300);
        }
    rt_log("online: fake bonus spots, play area %dx%d at %d,%d, %d walkable cells, %d candidates per kind",
           pa[2], pa[3], pa[0], pa[1], n, per);
    return 1;
}

// ---- chapter rankings (world map menu > Ranking, the last-chapter hub menu): top 100 unit + Emblem pairs

#define GOT_NET_STATE 0x62846b8        // slot of the class whose instance holds the online byte
#define NET_STATE_GET 0x1c3ca0         // its get_Instance(method)
#define NET_STATE_ONLINE 0x80          // the online byte the menus and map sequences test
#define RANK_PERSON_BY_INDEX 0x2377380 // Ranking.GetPersonDataByNetRankingIndex(i) static
#define RANK_GOD_BY_INDEX 0x23774b0    // Ranking.GetGodDataByNetRankingIndex(i) static
#define UNIT_FROM_PERSON 0x1c548d0     // UnitPool.GetFromPerson(PersonData, considerRelay) static
#define GOD_FROM_DATA 0x2334600        // GodPool.TryGet(GodData, includeReserved) static
#define NR_SUCCEEDED 0x19              // NexRanking.m_IsSucceeded

static uint8_t *net_online_byte(Cpu *c) {
    uint64_t slot = RD(G(GOT_NET_STATE));
    if (!slot) return 0;
    guest_call(c, G(INIT_METADATA), slot, 0, 0, 0);
    uint64_t cls = RD(slot), inst = cls ? guest_call(c, G(NET_STATE_GET), cls, 0, 0, 0) : 0;
    return inst ? (uint8_t *)(uintptr_t)(inst + NET_STATE_ONLINE) : 0;
}

// A Ranking menu item's ACall offers to connect when the online byte is clear; with it set for the call it opens the
// ranking screen. The byte is restored right after, so nothing else sees the game as online.
static int ranking_item(Cpu *c, const char *hook) {
    if (!cfg_int("TOTK_FAKE_ONLINE")) return 0;
    uint8_t *on = net_online_byte(c);
    if (!on) return 0;
    uint8_t was = *on;
    *on = 1;
    c->x[0] = hook_original(c, hook, c->x[0], c->x[1], c->x[2], c->x[3]);
    *on = was;
    return 1;
}
static int gmap_ranking_item(Cpu *c) { return ranking_item(c, "online_gmap_ranking_item"); }
static int hub_ranking_item(Cpu *c) { return ranking_item(c, "online_hub_ranking_item"); }

// NexRanking.Download(this, super, cid, Data[] out): 100 (person, god) net-ranking index pairs, best first, from the
// units and Emblems the player has. Seeded by the chapter id, so a chapter shows the same list each time.
static int ranking_download(Cpu *c) {
    if (!cfg_int("TOTK_FAKE_ONLINE")) return 0;
    uint64_t self = c->x[0], cid = c->x[2], out = c->x[3];
    c->x[0] = 1;
    *(uint8_t *)(uintptr_t)(self + NR_SUCCEEDED) = 1;
    if (!out) return 1;
    uint32_t len = *(const uint32_t *)(uintptr_t)(out + 0x18);
    int32_t *data = (int32_t *)(uintptr_t)(out + ARRAY_DATA);
    for (uint32_t i = 0; i < 2 * len; i++) data[i] = 0;

    int persons[46], gods[26], np = 0, ng = 0, npa = 0, nga = 0, all_p[46], all_g[26];
    for (int i = 1; i <= 45; i++) {
        uint64_t pd = guest_call(c, G(RANK_PERSON_BY_INDEX), (uint64_t)i, 0, 0, 0);
        if (!pd) continue;
        all_p[npa++] = i;
        if (guest_call(c, G(UNIT_FROM_PERSON), pd, 0, 0, 0)) persons[np++] = i;
    }
    for (int i = 1; i <= 25; i++) {
        uint64_t gd = guest_call(c, G(RANK_GOD_BY_INDEX), (uint64_t)i, 0, 0, 0);
        if (!gd) continue;
        all_g[nga++] = i;
        if (guest_call(c, G(GOD_FROM_DATA), gd, 0, 0, 0)) gods[ng++] = i;
    }
    if (np < 4) { for (int i = 0; i < npa; i++) persons[i] = all_p[i]; np = npa; }
    if (ng < 2) { for (int i = 0; i < nga; i++) gods[i] = all_g[i]; ng = nga; }
    if (!np || !ng) { rt_log("online: no ranking candidates"); return 1; }

    unsigned s = 2166136261u;   // FNV-1a over the chapter id's UTF-16 text
    if (cid) {
        int32_t n = *(const int32_t *)(uintptr_t)(cid + 0x10);
        const uint16_t *w = (const uint16_t *)(uintptr_t)(cid + 0x14);
        for (int32_t q = 0; q < n && q < 64; q++) s = (s ^ w[q]) * 16777619u;
    }
#define NEXT() (s = s * 1103515245u + 12345u, s >> 8)
    float wp[46], wg[26];   // popularity per unit / Emblem: a few favourites, a long tail
    for (int i = 0; i < np; i++) { float r = (float)(NEXT() % 1000) / 1000.f; wp[i] = r * r * r + 0.02f; }
    for (int i = 0; i < ng; i++) { float r = (float)(NEXT() % 1000) / 1000.f; wg[i] = r * r + 0.05f; }
    static float score[46 * 26];
    for (int i = 0; i < np; i++)
        for (int j = 0; j < ng; j++) score[i * ng + j] = wp[i] * wg[j] * (0.5f + (float)(NEXT() % 1000) / 1000.f);
#undef NEXT
    uint32_t n = 0;
    for (; n < len && n < (uint32_t)(np * ng); n++) {   // selection of the best remaining pair
        int best = 0;
        for (int k = 1; k < np * ng; k++)
            if (score[k] > score[best]) best = k;
        data[2 * n] = persons[best / ng];
        data[2 * n + 1] = gods[best % ng];
        score[best] = -1.f;
    }
    rt_log("online: fake ranking, %u pairs from %d units and %d Emblems", n, np, ng);
    return 1;
}

void engage_online_init(void) {
    hook_install("online_net_download", net_download);
    hook_install("online_kill_bonus_download", kill_bonus_download);
    hook_install("online_gmap_ranking_item", gmap_ranking_item);
    hook_install("online_hub_ranking_item", hub_ranking_item);
    hook_install("online_ranking_download", ranking_download);
}
