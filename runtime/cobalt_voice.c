// Cobalt's voice handling (the behaviour of Cobalt's audio/gamesound.rs, soundmanager.rs and soundplay.rs), as hooks
// listed in recomp/patches-engage-v200.txt:
// - a character's voice event plays "<event>_<person switch>" when a mod's bank has it (GameSound.PersonVoice,
//   UnitInfo.ReserveCharaVoice for V_Engage_Respond, GameSound.RingCleaningVoice for V_Ring*, and V_Pick as
//   "V_Pick_<person ascii name>");
// - a person switch "Mod!Fallback" plays the mod's events when loaded and the fallback person otherwise; Wwise and
//   the system voice loader get the fallback;
// - a Wwise marker "CobaltEvent:<event>_Original" in a mod's voice line plays the game's own line for that event.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cobalt_voice.h"
#include "hooks.h"
#include "host.h"
#include "platform/platform.h"

// main offsets (tools/il2cpp_map.py)
#define IS_EVENT_LOADED 0x2289000   // App.GameSound::IsEventLoaded(string)
#define UNIT_GET_PERSON 0x1a522c0   // App.Unit::get_Person
#define PERSON_ASCII 0x1f26160      // App.PersonData::get_AsciiName
#define CHAR_GET_SOUND 0x2b00c40    // Combat.Character::get_Sound
#define SOUND_PLAY_VOICE 0x25f0be0  // Combat.CharacterSound::PlayVoice(string)
#define MARKER_LABEL 0x2f26bd0      // AkMarkerCallbackInfo::get_strLabel

// ---- Il2CppString: klass, monitor, int32 length at 0x10, UTF-16 at 0x14
static int str_get(uint64_t s, char *out, size_t cap) {
    if (!s || !cap) return 0;
    int32_t n;
    memcpy(&n, (const void *)(uintptr_t)(s + 0x10), 4);
    if (n < 0) return 0;
    const uint16_t *w = (const uint16_t *)(uintptr_t)(s + 0x14);
    size_t k = 0;
    for (int32_t i = 0; i < n && k + 1 < cap; i++) out[k++] = w[i] < 0x80 ? (char)w[i] : '?';
    out[k] = 0;
    return 1;
}

static uint64_t g_string_klass;   // System.String's class, from the first game string seen

// A string in host memory, kept for the run; the same text gives the same string.
static uint64_t str_new(uint64_t klass, const char *t) {
    if (klass) g_string_klass = klass;
    else klass = g_string_klass;
    if (!klass) return 0;
    static struct { char text[128]; uint64_t s; } cache[1024];
    static int n;
    for (int i = 0; i < n; i++)
        if (!strcmp(cache[i].text, t)) return cache[i].s;
    size_t len = strlen(t);
    uint8_t *o = calloc(1, 0x14 + 4 * (len + 1));   // UTF-8 -> UTF-16 never needs more than 2 units a byte
    if (!o) return 0;
    memcpy(o, &klass, 8);
    uint16_t *w = (uint16_t *)(o + 0x14);
    int32_t units = 0;
    for (size_t i = 0; i < len;) {
        uint32_t ch = (uint8_t)t[i];
        int extra = ch < 0x80 ? 0 : ch < 0xE0 ? 1 : ch < 0xF0 ? 2 : 3;
        ch &= extra ? 0x3Fu >> extra : 0x7F;
        i++;
        for (int e = 0; e < extra && i < len; e++, i++) ch = ch << 6 | ((uint8_t)t[i] & 0x3F);
        if (ch >= 0x10000) ch -= 0x10000, w[units++] = (uint16_t)(0xD800 + (ch >> 10)), w[units++] = (uint16_t)(0xDC00 + (ch & 0x3FF));
        else w[units++] = (uint16_t)ch;
    }
    memcpy(o + 0x10, &units, 4);
    if (n < 1024 && len < sizeof cache->text) snprintf(cache[n].text, sizeof cache->text, "%s", t), cache[n++].s = (uint64_t)(uintptr_t)o;
    return (uint64_t)(uintptr_t)o;
}

static void note(const char *what, const char *event) {   // the first few, so a test can be read from the log
    static volatile int32_t n;
    if (plat_inc32(&n) <= 30) rt_log("cobalt: voice: %s %s", what, event);
}

static uint64_t klass_of(uint64_t s) { return s ? *(const uint64_t *)(uintptr_t)s : 0; }

uint64_t cobalt_string(uint64_t a_string, const char *text) { return str_new(klass_of(a_string), text); }

static int loaded(Cpu *c, uint64_t klass, const char *event) {
    uint64_t s = str_new(klass, event);
    return s && (guest_call(c, hook_main(IS_EVENT_LOADED), s, 0, 0, 0) & 1);
}

static int ends_with(const char *s, const char *t) {
    size_t n = strlen(s), k = strlen(t);
    return n >= k && !strcmp(s + n - k, t);
}

// "Mod!Fallback" -> "Fallback"; anything else unchanged.
static const char *switch_fallback(const char *person) {
    const char *b = strchr(person, '!');
    return b && !strchr(b + 1, '!') ? b + 1 : person;
}

static uint64_t g_character;                 // the last PersonVoice character, for _Original markers
static uint64_t g_pick_unit;                 // the unit of the last UnitPickVoice
static char g_pick_ascii[64];                // its person's ascii name
static volatile int32_t g_play_original_pick;

// GameSound.PersonVoice(gameobject, person, engage, event, character)
static int person_voice(Cpu *c) {
    char person[128], event[128], tmp[300];
    g_character = c->x[4];
    if (!str_get(c->x[1], person, sizeof person) || !str_get(c->x[3], event, sizeof event)) return 0;
    uint64_t k = klass_of(c->x[3]);
    if (ends_with(event, "_Original")) {   // a marker asked for the game's own line
        event[strlen(event) - 9] = 0;
        c->x[3] = str_new(k, event);
        note("game line for", event);
        return 0;
    }
    snprintf(tmp, sizeof tmp, "%s_%s", event, person);
    char parsed[300];
    snprintf(parsed, sizeof parsed, "%s", tmp);
    char *bang = strchr(tmp, '!');
    if (bang && !strchr(bang + 1, '!')) {   // "<event>_Mod!Fallback"
        *bang = 0;
        char *under = strrchr(tmp, '_');
        if (loaded(c, k, tmp)) snprintf(parsed, sizeof parsed, "%s", tmp);
        else if (under) *under = 0, snprintf(parsed, sizeof parsed, "%s_%s", tmp, bang + 1);
    }
    const char *sw = switch_fallback(person);
    int has = loaded(c, k, parsed);
    if (strcmp(sw, person)) c->x[1] = str_new(klass_of(c->x[1]), sw);
    if (has) c->x[3] = str_new(k, parsed), note("mod event", parsed);
    return 0;
}

// SoundManager.SetEnumParam(this, valueName, value, gameObject): the Person switch gets the fallback
static int set_enum_param(Cpu *c) {
    char name[64], value[128];
    if (!str_get(c->x[1], name, sizeof name) || strcmp(name, "Person") || !str_get(c->x[2], value, sizeof value))
        return 0;
    const char *f = switch_fallback(value);
    if (strcmp(f, value)) c->x[2] = str_new(klass_of(c->x[2]), f);
    return 0;
}

// UnitInfo.ReserveCharaVoice(side, person, engage, event)
static int reserve_chara_voice(Cpu *c) {
    char person[128], event[128], m[300];
    if (!str_get(c->x[1], person, sizeof person) || !str_get(c->x[3], event, sizeof event)) return 0;
    if (strcmp(event, "V_Engage_Respond")) return 0;
    snprintf(m, sizeof m, "%s_%s", event, person);
    if (loaded(c, klass_of(c->x[3]), m)) c->x[3] = str_new(klass_of(c->x[3]), m);
    return 0;
}

// GameSound.RingCleaningVoice(person, event, character)
static int ring_voice(Cpu *c) {
    char person[128], event[128], m[300];
    if (!str_get(c->x[0], person, sizeof person) || !str_get(c->x[1], event, sizeof event)) return 0;
    if (!strstr(event, "V_Ring")) return 0;
    snprintf(m, sizeof m, "%s_%s", event, person);
    if (loaded(c, klass_of(c->x[1]), m)) c->x[1] = str_new(klass_of(c->x[1]), m);
    return 0;
}

// GameSound.UnitPickVoice(unit): remember the unit and its person's ascii name for V_Pick
static int unit_pick_voice(Cpu *c) {
    uint64_t unit = c->x[0];
    g_pick_unit = unit, g_pick_ascii[0] = 0;
    if (!unit) return 0;
    uint64_t person = guest_call(c, hook_main(UNIT_GET_PERSON), unit, 0, 0, 0);
    uint64_t ascii = person ? guest_call(c, hook_main(PERSON_ASCII), person, 0, 0, 0) : 0;
    str_get(ascii, g_pick_ascii, sizeof g_pick_ascii);
    return 0;
}

// SoundManager.PostEventWithTemporaryGameObject(this, event, gameObject, character, isGetPosition)
static int post_event_temp(Cpu *c) {
    char event[64], m[160];
    if (!str_get(c->x[1], event, sizeof event) || strcmp(event, "V_Pick")) return 0;
    if (plat_xchg32(&g_play_original_pick, 0)) return 0;
    if (!g_pick_ascii[0]) return 0;
    snprintf(m, sizeof m, "V_Pick_%s", g_pick_ascii);
    if (loaded(c, klass_of(c->x[1]), m)) c->x[1] = str_new(klass_of(c->x[1]), m), note("mod event", m);
    return 0;
}

// SoundPlay.PostEventCallback(this, cookie, type, info): Wwise markers "CobaltEvent:<event>"
static int post_event_callback(Cpu *c) {
    if ((uint32_t)c->x[2] != 4 || !c->x[3]) return 0;   // AkCallbackType.Marker
    uint64_t label = guest_call(c, hook_main(MARKER_LABEL), c->x[3], 0, 0, 0);
    char l[160];
    if (!str_get(label, l, sizeof l) || strncmp(l, "CobaltEvent:", 12)) return 0;
    const char *event = l + 12;
    note("marker", event);
    if (!strcmp(event, "V_Pick_Original")) {
        if (g_pick_unit) {
            plat_xchg32(&g_play_original_pick, 1);
            hook_original(c, "cobalt_unit_pick_voice", g_pick_unit, 0, 0, 0);
        }
    } else if (ends_with(event, "_Original") && g_character) {
        uint64_t ch = g_character;
        g_character = 0;
        uint64_t snd = guest_call(c, hook_main(CHAR_GET_SOUND), ch, 0, 0, 0);
        if (snd) guest_call(c, hook_main(SOUND_PLAY_VOICE), snd, str_new(klass_of(label), event), 0, 0);
    }
    return 0;
}

// GameSound.LoadSystemVoice / UnloadSystemVoice(person)
static int system_voice(Cpu *c) {
    char person[128];
    if (!str_get(c->x[0], person, sizeof person)) return 0;
    const char *f = switch_fallback(person);
    if (strcmp(f, person)) c->x[0] = str_new(klass_of(c->x[0]), f);
    return 0;
}

void cobalt_voice_init(void) {
    int ok = hook_install("cobalt_person_voice", person_voice) & hook_install("cobalt_set_enum_param", set_enum_param) &
             hook_install("cobalt_reserve_chara_voice", reserve_chara_voice) & hook_install("cobalt_ring_voice", ring_voice) &
             hook_install("cobalt_unit_pick_voice", unit_pick_voice) &
             hook_install("cobalt_post_event_temp", post_event_temp) &
             hook_install("cobalt_post_event_callback", post_event_callback) &
             hook_install("cobalt_load_system_voice", system_voice) &
             hook_install("cobalt_unload_system_voice", system_voice);
    rt_log("cobalt: voice hooks %s", ok ? "on" : "partly missing from this build");
}
