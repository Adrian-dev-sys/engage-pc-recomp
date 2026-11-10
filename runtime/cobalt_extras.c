// Cobalt's extras (its lib.rs, combatui.rs, ringvibration.rs, save.rs and config/gamemode.rs): combat damage popups and
// combat UI switches, ring polish rumble, the game mode (Phoenix included) of the loaded save, the Phoenix label in
// the save list and engage zone remapping. The switches live in F1 > Mods (engage.ini) instead of Cobalt's in-game page.
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "cobalt_extras.h"
#include "cobalt_voice.h"
#include "hooks.h"
#include "host.h"
#include "platform/platform.h"

// main offsets (tools/il2cpp_map.py, tools/il2cpp_usages.py)
#define INIT_METADATA 0x492060
#define SINGLETON_GET 0x1c3b70         // SingletonClass<T>.get_Instance(method)
#define GOT_GAMEUSERDATA 0x62840f0     // address of SingletonClass<GameUserData>.get_Instance's MethodInfo slot
#define USERDATA_MODE 0x2c             // GameUserData.m_GameMode
#define HEADER_GET 0x1e6b8f0           // GameSaveDataHeaderReader.Handle.get_Header
#define HEADER_MODE 0x2284710          // GameSaveDataHeader.get_GameMode
#define TMP_SET_TEXT 0x28317d0
#define GRAPHIC_SET_COLOR 0x318c030    // Graphic.set_color(Color)
#define MESS_GET 0x25c9240             // Mess.Get(string) static
#define PAD_VIBRATION 0x1f23ce0        // Pad.Vibration() static -> VibrationManager
#define ONE_SHOT 0x21c02c0             // VibrationManager.OneShot(time, magnitude, amp_low, amp_high, freq_low, freq_high)
#define GODPOOL_TRY_GET 0x2334570      // GodPool.TryGet(gid, bool) static -> GodUnit
#define GODUNIT_DATA 0x233eb20
#define GOD_NICKNAME 0x232d930
#define GOD_ASCII 0x232ddd0
#define GOD_GID 0x232d8f0
#define SLOT_GOD_LIST 0x617dc98        // StructData<GodData>.GetList
#define PHOENIX 2

#define RD(a) (*(const uint64_t *)(uintptr_t)(a))
#define G(off) hook_main(off)

static uint64_t g_user_data;   // the GameUserData singleton, once a hook has seen it

static void utf8(uint64_t s, char *out, size_t cap) {   // a game string as UTF-8 (BMP)
    out[0] = 0;
    if (!s) return;
    int32_t len = *(const int32_t *)(uintptr_t)(s + 0x10);
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

static void set_f(Cpu *c, int i, float f) {
    uint32_t b;
    memcpy(&b, &f, 4);
    c->v[i] = b;
}

void cobalt_extras_capture(Cpu *c) {
    if (g_user_data) return;
    uint64_t slot = RD(G(GOT_GAMEUSERDATA));
    if (!slot) return;
    guest_call(c, G(INIT_METADATA), slot, 0, 0, 0);
    uint64_t mi = RD(slot);
    g_user_data = mi ? guest_call(c, G(SINGLETON_GET), mi, 0, 0, 0) : 0;
}

int cobalt_game_mode(void) {
    return g_user_data ? *(const int32_t *)(uintptr_t)(g_user_data + USERDATA_MODE) : -1;
}

void cobalt_set_game_mode(int mode) {   // a plain field store (GameUserData.SetGameMode does only that)
    if (g_user_data && mode >= 0 && mode <= PHOENIX) *(int32_t *)(uintptr_t)(g_user_data + USERDATA_MODE) = mode;
}

// ---- combat

static int damage_popup(Cpu *c) {   // Combat.HUDPopupGroup.DamagePopup(phase) static
    (void)c;
    return !cfg_int("TOTK_COBALT_COMBAT_POPUPS");
}

static int fade_in_hud(Cpu *c) {   // Combat.CombatWorld.FadeInHUD(this)
    cobalt_extras_capture(c);
    return !cfg_int("TOTK_COBALT_COMBAT_UI");
}

// ---- ring polish rumble, through the game's own vibration (Pad.Vibration().OneShot). Same events as Cobalt's
// ringvibration.rs; the strengths and lengths are our own.

static void vibrate(Cpu *c, float time, float magnitude, float amp_low, float amp_high) {
    if (!cfg_int("TOTK_COBALT_RING_RUMBLE")) return;
    uint64_t vm = guest_call(c, G(PAD_VIBRATION), 0, 0, 0, 0);
    if (!vm) return;
    set_f(c, 0, time), set_f(c, 1, magnitude), set_f(c, 2, amp_low), set_f(c, 3, amp_high);
    set_f(c, 4, 160.f), set_f(c, 5, 320.f);   // the Switch's neutral low and high band frequencies
    guest_call(c, G(ONE_SHOT), vm, 0, 0, 0);
}

static int clean_ring(Cpu *c) {   // RingCleaningSequence.CleanRing(this, strength, hitResult): a spot polished
    int strong = (int)c->x[1] == 0, hit = (int)c->x[2];   // Strength: Strong 0, Weak 1; HitResult: NotHit, Near, Hit
    hook_original(c, "cobalt_clean_ring", c->x[0], c->x[1], c->x[2], c->x[3]);
    if (hit < 0 || hit > 2) return 1;
    // our own tuning: a better hit is stronger and longer, a strong polish 1.6x; the light motor leads
    float a = (0.12f + 0.18f * (float)hit) * (strong ? 1.6f : 1.f);
    vibrate(c, 0.08f + 0.06f * (float)hit, 1.f, a * 0.6f, a > 1 ? 1 : a);
    return 1;
}

static int rub_effect(Cpu *c) {   // RingCleaning.EffectController.PlayRubEffect() static: R held, stick turning
    c->x[0] = hook_original(c, "cobalt_rub_effect", c->x[0], 0, 0, 0);
    uint64_t r = c->x[0];
    vibrate(c, 0.1f, 1.f, 0.03f, 0.07f);   // a faint buzz while rubbing
    c->x[0] = r;
    return 1;
}

static int cleaning_start(Cpu *c) {   // RingCleaningClothAnimationEvent.CleaningStartEvent(this): A pressed
    hook_original(c, "cobalt_cleaning_start", c->x[0], c->x[1], 0, 0);
    vibrate(c, 0.06f, 1.f, 0.04f, 0.08f);   // a short tick
    return 1;
}

// ---- AssetTable.Result.GetForDemo(pid, isDefault, isPlain) static: scenes such as supports ask for the default outfit;
// with the switch on they get the one the character wears (Cobalt's support.rs).

static int demo_outfit(Cpu *c) {
    if (cfg_int("TOTK_COBALT_SUPPORT_OUTFIT")) c->x[1] = 0;
    return 0;
}

// Ring polishing clears the unit's accessories before building its model (patch set ringoutfit); the same switch skips
// that. Called on every settings change (gfx_present.c window_settings_live).
void cobalt_outfit_live(void) {
    if (!RT_ENGAGE) return;
    static int set = -2, shown = -1;
    if (set == -2) set = rt_patch_set("ringoutfit");
    int on = cfg_int("TOTK_COBALT_SUPPORT_OUTFIT") != 0;
    if (set < 0 || on == shown) return;
    rt_patch_switch(set, on);
    shown = on;
}

// ---- save list: a Phoenix save says so (SaveDataMenu.MenuItemContent.SetupByMenuItem(this, item))

static int save_item(Cpu *c) {
    uint64_t self = c->x[0], item = c->x[1];
    hook_original(c, "cobalt_save_item", self, item, c->x[2], 0);
    cobalt_extras_capture(c);
    uint64_t handle = item ? RD(item + 0x68) : 0;   // m_SaveDataHeaderHandle
    uint64_t header = handle ? guest_call(c, G(HEADER_GET), handle, 0, 0, 0) : 0;
    if (!header || (int)guest_call(c, G(HEADER_MODE), header, 0, 0, 0) != PHOENIX) return 1;
    uint64_t text = RD(self + 0xa8), image = RD(self + 0xb0);   // m_ModeText, m_GameModeImage
    uint64_t label = guest_call(c, G(MESS_GET), cobalt_string(0, "MID_SYS_Mode_Phoenix"), 0, 0, 0);
    if (text && label) guest_call(c, G(TMP_SET_TEXT), text, label, 0, 0);
    if (image) {
        set_f(c, 0, 0.69f), set_f(c, 1, 0.043f), set_f(c, 2, 0.412f), set_f(c, 3, 1.f);
        guest_call(c, G(GRAPHIC_SET_COLOR), image, 0, 0, 0);
    }
    return 1;
}

// ---- GodData.GetEngageZonePrefabPath(gid) static: an emblem whose nickname is X_Y_<Ascii> uses that emblem's zone

static int engage_zone(Cpu *c) {
    uint64_t gid = c->x[0];
    uint64_t unit = gid ? guest_call(c, G(GODPOOL_TRY_GET), gid, 0, 0, 0) : 0;
    uint64_t data = unit ? guest_call(c, G(GODUNIT_DATA), unit, 0, 0, 0) : 0;
    char nick[160], want[160], own[160];
    utf8(data ? guest_call(c, G(GOD_NICKNAME), data, 0, 0, 0) : 0, nick, sizeof nick);
    const char *p = strchr(nick, '_');
    p = p ? strchr(p + 1, '_') : NULL;
    if (!p) return 0;
    snprintf(want, sizeof want, "%s", p + 1);
    char *e = strchr(want, '_');
    if (e) *e = 0;
    if (!want[0]) return 0;
    uint64_t mi = G(SLOT_GOD_LIST);
    guest_call(c, G(INIT_METADATA), mi, 0, 0, 0);
    mi = RD(mi);
    uint64_t list = mi ? guest_call(c, RD(mi), mi, 0, 0, 0) : 0;
    uint64_t items = list ? RD(list + 0x10) : 0;
    int32_t n = list ? *(const int32_t *)(uintptr_t)(list + 0x18) : 0;
    utf8(gid, own, sizeof own);
    for (int32_t i = 0; items && i < n; i++) {
        uint64_t god = RD(items + 0x20 + 8 * (uint64_t)i);
        char ascii[160];
        utf8(god ? guest_call(c, G(GOD_ASCII), god, 0, 0, 0) : 0, ascii, sizeof ascii);
        if (strcmp(ascii, want)) continue;
        uint64_t remapped = guest_call(c, G(GOD_GID), god, 0, 0, 0);
        char r[160];
        utf8(remapped, r, sizeof r);
        if (!remapped || !strcmp(r, own)) return 0;
        static volatile int32_t logged;
        if (plat_inc32(&logged) <= 10) rt_log("cobalt: engage zone of %s is %s's", own, r);
        c->x[0] = hook_original(c, "cobalt_engage_zone", remapped, c->x[1], 0, 0);
        return 1;
    }
    return 0;
}

void cobalt_extras_init(void) {
    hook_install("cobalt_damage_popup", damage_popup);
    hook_install("cobalt_fade_in_hud", fade_in_hud);
    hook_install("cobalt_clean_ring", clean_ring);
    hook_install("cobalt_rub_effect", rub_effect);
    hook_install("cobalt_cleaning_start", cleaning_start);
    hook_install("cobalt_save_item", save_item);
    hook_install("cobalt_engage_zone", engage_zone);
    hook_install("cobalt_demo_outfit", demo_outfit);
}
