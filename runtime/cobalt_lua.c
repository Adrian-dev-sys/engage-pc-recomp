// Cobalt mods' root-level Lua scripts (the behaviour of Cobalt's script.rs): each <mod>/<Name>.lua runs when a map
// opens in the Somniel (the hub). Its code is run in the event script's Lua state, then its function <Name> (the file
// name, same case) is started as a coroutine of the opening sequence. Hook cobalt_map_opening on
// App.EventSequence::MapOpening(parent).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cobalt_lua.h"
#include "cobalt_voice.h"
#include "dispatch.h"
#include "hooks.h"
#include "host.h"
#include "patches.h"
#include "platform/platform.h"

// main offsets (tools/il2cpp_map.py)
#define SINGLETON_GET 0x1c3b70        // SingletonClass<T>.get_Instance(method), the generic body
#define GAMEUSERDATA_METHOD 0x62840f0 // the metadata slot of SingletonClass<GameUserData>.get_Instance's MethodInfo
#define INIT_METADATA 0x492060        // il2cpp's metadata-slot initialiser
#define EVENTSCRIPT_INSTANCE 0x24e2c70 // App.EventScript::get_Instance
#define EVENTSCRIPT_NAME 0x24e1ce0    // App.EventScript::get_Name (a game string, for String's class)
#define TABLE_OWNER 0x2d3ea20         // MoonSharp.Interpreter.Table::get_OwnerScript
#define TABLE_GET 0x2d26b40           // MoonSharp.Interpreter.Table::Get(string)
#define SCRIPT_DOSTRING 0x2d2cf90     // MoonSharp.Interpreter.Script::DoString(code, globals, name)
#define PROC_CHILD 0x281eae0          // App.ProcInst::GetChild
#define ADD_COROUTINE 0x24e3490       // App.EventSequence::AddCoroutine(func, args)
#define SEQUENCE_HUB 4                // App.GameUserData.Sequences.Hub
#define DYN_FUNCTION 5                // MoonSharp DataType.Function (DynValue +0x30)
#define DYN_STRING 4                  // MoonSharp DataType.String
#define DYN_GET_STRING 0x2e24420      // MoonSharp.Interpreter.DynValue::get_String

typedef struct { char file[2600], name[128]; uint64_t code; } Script;
static Script scripts[64];
static int nscripts;

void cobalt_lua_add(const char *file, const char *name) {
    if (nscripts < (int)(sizeof scripts / sizeof *scripts)) {
        snprintf(scripts[nscripts].file, sizeof scripts->file, "%s", file);
        snprintf(scripts[nscripts++].name, sizeof scripts->name, "%s", name);
    }
}

static uint64_t ld64(uint64_t a) { return *(const uint64_t *)(uintptr_t)a; }

static int sequence(Cpu *c) {
    uint64_t slot = ld64(hook_main(GAMEUSERDATA_METHOD));
    if (!slot) return -1;
    guest_call(c, hook_main(INIT_METADATA), slot, 0, 0, 0);
    uint64_t method = ld64(slot);
    uint64_t ud = method ? guest_call(c, hook_main(SINGLETON_GET), method, 0, 0, 0) : 0;
    return ud ? *(const int32_t *)(uintptr_t)(ud + 0x28) : -1;
}

static int map_opening(Cpu *c) {
    uint64_t parent = c->x[0];
    hook_original(c, "cobalt_map_opening", parent, 0, 0, 0);
    if (!nscripts || sequence(c) != SEQUENCE_HUB) return 1;
    uint64_t es = guest_call(c, hook_main(EVENTSCRIPT_INSTANCE), 0, 0, 0, 0);
    uint64_t globals = es ? ld64(es + 0x28) : 0;
    uint64_t script = globals ? guest_call(c, hook_main(TABLE_OWNER), globals, 0, 0, 0) : 0;
    uint64_t child = guest_call(c, hook_main(PROC_CHILD), parent, 0, 0, 0);
    if (!script || !child) {
        rt_log("cobalt: lua: no event script or child sequence at the hub's map opening");
        return 1;
    }
    uint64_t any = guest_call(c, hook_main(EVENTSCRIPT_NAME), es, 0, 0, 0);
    for (int i = 0; i < nscripts; i++) {
        if (!scripts[i].code) {   // read once; the string stays for the run
            uint64_t n = 0;
            char *code = (char *)plat_file_read(scripts[i].file, &n);
            char *text = code ? realloc(code, n + 1) : NULL;
            if (!text) {
                free(code);
                continue;
            }
            text[n] = 0;
            scripts[i].code = cobalt_string(any, text);
            free(text);
        }
        uint64_t s = scripts[i].code;
        if (!s) continue;
        guest_call6(c, hook_main(SCRIPT_DOSTRING), script, s, 0, 0, 0, 0);
        uint64_t fn = guest_call(c, hook_main(TABLE_GET), globals, cobalt_string(any, scripts[i].name), 0, 0);
        if (!fn || *(const int32_t *)(uintptr_t)(fn + 0x30) != DYN_FUNCTION) {
            rt_log("cobalt: lua: %s defines no function %s", scripts[i].file, scripts[i].name);
            continue;
        }
        guest_call(c, hook_main(ADD_COROUTINE), child, fn, 0, 0);
        rt_log("cobalt: lua: %s started", scripts[i].name);
    }
    return 1;
}

// App.ScriptSystem::Log(args) is empty in the release game; Cobalt prints it, so mods' scripts can log.
static int script_log(Cpu *c) {
    uint64_t arr = c->x[0];
    if (!arr) return 1;
    int32_t n = *(const int32_t *)(uintptr_t)(arr + 0x18);
    char line[512];
    size_t k = 0;
    for (int32_t i = 0; i < n && i < 16; i++) {
        uint64_t dv = ld64(arr + 0x20 + 8 * (uint64_t)i);
        if (!dv || *(const int32_t *)(uintptr_t)(dv + 0x30) != DYN_STRING) continue;
        uint64_t s = guest_call(c, hook_main(DYN_GET_STRING), dv, 0, 0, 0);
        if (!s) continue;
        int32_t len = *(const int32_t *)(uintptr_t)(s + 0x10);
        const uint16_t *w = (const uint16_t *)(uintptr_t)(s + 0x14);
        for (int32_t q = 0; q < len && k + 2 < sizeof line; q++) line[k++] = w[q] < 0x80 ? (char)w[q] : '?';
        if (k + 2 < sizeof line) line[k++] = ' ';
    }
    line[k] = 0;
    rt_log("lua: %s", line);
    return 1;
}

// ---- Cobalt's Lua functions. The game registers its actions with EventScript.RegistAction(this, delegate, name)
// (functions, which return a value, with RegistFunction), which puts the delegate in the Lua globals; MoonSharp calls
// it through reflection, i.e. through the delegate's MethodInfo: methodPointer(DynValue[] args, method). A Cobalt
// action is a copy of a game delegate and its MethodInfo whose methodPointer is a host function at a fixed address on
// the patch page (registered before the dispatch table is sealed).
#define REGIST_ACTION 0x24e2430   // App.EventScript::RegistAction(delegate, name)
#define REGIST_FUNCTION 0x24e23e0 // App.EventScript::RegistFunction(delegate, name)
#define HOST_ACTIONS (RT_PATCH_PAGE + 0x800)   // 16 bytes each; ov_zstd.c uses +0xfe0 and +0xff0

// main offsets of what the actions call (tools/il2cpp_map.py)
#define TRY_UNIT 0x21961f0         // ScriptUtil.TryGetUnit(args, i, bool) static
#define TRY_STRING 0x2196920       // ScriptUtil.TryGetString(args, i, default) static
#define TRY_INT 0x2196cd0          // ScriptUtil.TryGetInt(args, i, default) static
#define TRY_ITEM 0x21a3740         // ScriptUtil.TryGetItem(args, i, bool) static
#define GET_SEQUENCE 0x21a4860     // ScriptUtil.GetSequence() static
#define YIELD 0x21994e0            // ScriptUtil.Yield() static
#define NOTICE_ADD 0x1f1b540       // NoticeManager.Add(Kinds, string) static
#define NOTICE_FACILITY 1          // NoticeManager.Kinds.Facility
#define UNIT_ITEM_ADD 0x1a0c990    // Unit.ItemAdd(string iid)
#define TRANSPORTER_ADD 0x22a2040  // Transporter.Add(ItemData) static
#define UNIT_SET_LEVEL 0x1a0e1d0   // Unit.set_Level(int)
#define UNIT_LEARN_SKILL 0x1a3c290 // Unit.LearnJobSkill()
#define GIFT_GET 0x23f38a0         // HubSequence.GiftGet(reward, message)
#define ITEM_SHOP 0x28ab140        // HubItemShopSequence.CreateBind(ProcInst) static
#define WEAPON_SHOP 0x2a744f0      // HubWeaponShopSequence.CreateBind(ProcInst) static
#define REFINE_SHOP 0x23dccb0      // HubRefineShopSequence.CreateBind(ProcInst) static
#define HAS_CONTENT 0x29f4270      // DLCManager.HasContent(Content) static; Content.E0 = 0, the expansion pass
#define RING_ADD 0x1c5d420         // UnitRingPool.Add(rnid, Unit, count) static
#define NEW_BOOLEAN 0x2e200f0      // DynValue.NewBoolean(bool) static
#define G(off) hook_main(off)

static void trace(const char *what) {   // the first calls of each kind, so a test can be read from the log
    static volatile int32_t n;
    if (plat_inc32(&n) <= 40) rt_log("cobalt: lua: %s called", what);
}

// The i-th argument as UTF-8 ("" when it is not a string).
static void arg_str(Cpu *c, uint64_t args, int i, char *out, size_t cap) {
    out[0] = 0;
    if (!args || i >= *(const int32_t *)(uintptr_t)(args + 0x18)) return;
    uint64_t dv = ld64(args + 0x20 + 8 * (uint64_t)i);
    if (!dv || *(const int32_t *)(uintptr_t)(dv + 0x30) != DYN_STRING) return;
    uint64_t s = guest_call(c, G(DYN_GET_STRING), dv, 0, 0, 0);
    if (!s) return;
    int32_t len = *(const int32_t *)(uintptr_t)(s + 0x10);
    const uint16_t *w = (const uint16_t *)(uintptr_t)(s + 0x14);
    size_t k = 0;
    for (int32_t q = 0; q < len && k + 5 < cap; q++) {
        uint32_t ch = w[q];
        if (ch >= 0xD800 && ch < 0xDC00 && q + 1 < len) ch = 0x10000 + ((ch - 0xD800) << 10) + (w[++q] - 0xDC00u);
        if (ch < 0x80) {
            out[k++] = (char)ch;
        } else if (ch < 0x800) {
            out[k++] = (char)(0xC0 | ch >> 6), out[k++] = (char)(0x80 | (ch & 0x3F));
        } else if (ch < 0x10000) {
            out[k++] = (char)(0xE0 | ch >> 12), out[k++] = (char)(0x80 | (ch >> 6 & 0x3F));
            out[k++] = (char)(0x80 | (ch & 0x3F));
        } else {
            out[k++] = (char)(0xF0 | ch >> 18), out[k++] = (char)(0x80 | (ch >> 12 & 0x3F));
            out[k++] = (char)(0x80 | (ch >> 6 & 0x3F)), out[k++] = (char)(0x80 | (ch & 0x3F));
        }
    }
    out[k] = 0;
}

static void act_notice(Cpu *c) {   // CobaltNotice(message)
    trace("CobaltNotice");
    char text[512];
    arg_str(c, c->x[0], 0, text, sizeof text);
    rt_log("cobalt: lua: CobaltNotice: %s", text);
    uint64_t msg = guest_call(c, G(TRY_STRING), c->x[0], 0, 0, 0);
    if (msg) guest_call(c, G(NOTICE_ADD), NOTICE_FACILITY, msg, 0, 0);
}

static void act_item_gain_silent(Cpu *c) {   // ItemGainSilent(pid | nil, iid)
    trace("ItemGainSilent");
    uint64_t args = c->x[0], unit = guest_call(c, G(TRY_UNIT), args, 0, 0, 0);
    if (unit) {
        uint64_t iid = guest_call(c, G(TRY_STRING), args, 1, 0, 0);
        if (iid) guest_call(c, G(UNIT_ITEM_ADD), unit, iid, 0, 0);
    } else {   // no unit: the convoy
        uint64_t item = guest_call(c, G(TRY_ITEM), args, 1, 0, 0);
        if (item) guest_call(c, G(TRANSPORTER_ADD), item, 0, 0, 0);
        guest_call(c, G(YIELD), 0, 0, 0, 0);
    }
}

static void act_unit_set_level(Cpu *c) {   // UnitSetLevel(pid, level)
    trace("UnitSetLevel");
    uint64_t args = c->x[0], unit = guest_call(c, G(TRY_UNIT), args, 0, 0, 0);
    uint64_t level = guest_call(c, G(TRY_INT), args, 1, 1, 0);
    if (unit) guest_call(c, G(UNIT_SET_LEVEL), unit, (uint32_t)level, 0, 0);
}

static void act_unit_learn_job_skill(Cpu *c) {   // UnitLearnJobSkill(pid)
    trace("UnitLearnJobSkill");
    uint64_t unit = guest_call(c, G(TRY_UNIT), c->x[0], 0, 0, 0);
    if (unit) guest_call(c, G(UNIT_LEARN_SKILL), unit, 0, 0, 0);
}

// Generic methods and classes through the metadata-usage slots the game uses (tools/il2cpp_usages.py): il2cpp
// initialises a slot, which then holds the MethodInfo (whose methodPointer comes first) or the class.
#define SLOT_JOBDATA_GET 0x617df48   // StructData<JobData>.Get(string) static
#define SLOT_WEAPONMASK 0x615c608    // class WeaponMask
#define OBJECT_NEW 0x4921f0          // il2cpp object allocation (class)
#define WEAPONMASK_CTOR 0x21ca080    // WeaponMask..ctor(int)
#define UNIT_CLASS_CHANGE 0x1a3c7b0  // Unit.ClassChange(JobData, ItemData)
#define UNIT_SET_WEAPON 0x1a3cd50    // Unit.SetSelectedWeapon(WeaponMask)

static uint64_t slot(Cpu *c, uint64_t off) {
    guest_call(c, G(INIT_METADATA), G(off), 0, 0, 0);
    return ld64(G(off));
}

static void act_unit_set_job(Cpu *c) {   // UnitSetJob(pid, jid[, weapon mask 1..0x3FF])
    trace("UnitSetJob");
    uint64_t args = c->x[0], unit = guest_call(c, G(TRY_UNIT), args, 0, 0, 0);
    uint64_t jid = guest_call(c, G(TRY_STRING), args, 1, 0, 0);
    uint64_t get = slot(c, SLOT_JOBDATA_GET), job = jid && get ? guest_call(c, ld64(get), jid, get, 0, 0) : 0;
    char name[128];
    arg_str(c, args, 1, name, sizeof name);
    if (!unit || !job) {
        rt_log("cobalt: lua: UnitSetJob: no %s", !unit ? "unit" : name);
        return;
    }
    guest_call(c, G(UNIT_CLASS_CHANGE), unit, job, 0, 0);
    int32_t mask = (int32_t)guest_call(c, G(TRY_INT), args, 2, 0x7fffffff, 0);
    if (mask >= 1 && mask <= 0x3FF) {
        uint64_t klass = slot(c, SLOT_WEAPONMASK), wm = klass ? guest_call(c, G(OBJECT_NEW), klass, 0, 0, 0) : 0;
        if (wm) {
            guest_call(c, G(WEAPONMASK_CTOR), wm, (uint32_t)mask, 0, 0);
            guest_call(c, G(UNIT_SET_WEAPON), unit, wm, 0, 0);
        }
    }
}

static void act_item_gift(Cpu *c) {   // ItemGift(reward id, message id)
    trace("ItemGift");
    uint64_t args = c->x[0], seq = guest_call(c, G(GET_SEQUENCE), 0, 0, 0, 0);
    uint64_t reward = guest_call(c, G(TRY_STRING), args, 0, 0, 0), mess = guest_call(c, G(TRY_STRING), args, 1, 0, 0);
    if (seq && reward) guest_call(c, G(GIFT_GET), seq, reward, mess, 0);
    guest_call(c, G(YIELD), 0, 0, 0, 0);
}

static void shop(Cpu *c, uint64_t create) {
    trace("Menu*ShopShow");
    uint64_t seq = guest_call(c, G(GET_SEQUENCE), 0, 0, 0, 0);
    if (seq) guest_call(c, G(create), seq, 0, 0, 0);
    guest_call(c, G(YIELD), 0, 0, 0, 0);
}
static void act_item_shop(Cpu *c) { shop(c, ITEM_SHOP); }
static void act_weapon_shop(Cpu *c) { shop(c, WEAPON_SHOP); }
static void act_refine_shop(Cpu *c) { shop(c, REFINE_SHOP); }

static void act_add_bond_ring(Cpu *c) {   // AddBondRing(rnid[, count])
    trace("AddBondRing");
    uint64_t args = c->x[0], rnid = guest_call(c, G(TRY_STRING), args, 0, 0, 0);
    int64_t n = (int32_t)guest_call(c, G(TRY_INT), args, 1, 1, 0);
    if (rnid) guest_call(c, G(RING_ADD), rnid, 0, (uint64_t)(n < 1 ? 1 : n), 0);
}

static void fn_has_season_pass(Cpu *c) {   // HasPurchasedSeasonPass() -> bool: a Lua function, not an action
    trace("HasPurchasedSeasonPass");
    uint64_t has = guest_call(c, G(HAS_CONTENT), 0, 0, 0, 0) & 1;
    c->x[0] = guest_call(c, G(NEW_BOOLEAN), has, 0, 0, 0);
}

typedef struct { const char *name; GuestFn fn; int function; } Action;
static const Action actions[] = {
    {"CobaltNotice", act_notice, 0},
    {"ItemGainSilent", act_item_gain_silent, 0},
    {"UnitSetLevel", act_unit_set_level, 0},
    {"UnitLearnJobSkill", act_unit_learn_job_skill, 0},
    {"UnitSetJob", act_unit_set_job, 0},
    {"ItemGift", act_item_gift, 0},
    {"MenuItemShopShow", act_item_shop, 0},
    {"MenuWeaponShopShow", act_weapon_shop, 0},
    {"MenuRefineShopShow", act_refine_shop, 0},
    {"AddBondRing", act_add_bond_ring, 0},
    {"HasPurchasedSeasonPass", fn_has_season_pass, 1},
};
#define NACTIONS (sizeof actions / sizeof *actions)

__attribute__((constructor)) static void actions_register(void) {
    for (size_t i = 0; i < NACTIONS; i++) rt_register_host(HOST_ACTIONS + 16 * i, actions[i].fn);
}

// A game action / function delegate seen at its registration, copied at once: the delegate itself may be freed when
// the game drops that script, and its memory reused.
static uint8_t g_tdel[2][0x68], g_tmeth[2][0x58];
static uint64_t g_template, g_ftemplate;   // nonzero once copied; the copies' addresses

// Only a static method with one parameter (DynValue[] args) and no target will do: a lambda with a closure target gets
// the target first and the arguments one register later (Il2CppDelegate: target +0x20, method +0x28; MethodInfo:
// flags +0x44, parameters_count +0x4a).
static int plain_static(uint64_t d) {
    if (!d || ld64(d + 0x20)) return 0;
    uint64_t m = ld64(d + 0x28);
    return m && (*(const uint16_t *)(uintptr_t)(m + 0x44) & 0x10) && *(const uint8_t *)(uintptr_t)(m + 0x4a) == 1;
}
static uint64_t keep_template(int k, uint64_t d) {
    memcpy(g_tdel[k], (const void *)(uintptr_t)d, sizeof g_tdel[k]);
    memcpy(g_tmeth[k], (const void *)(uintptr_t)ld64(d + 0x28), sizeof g_tmeth[k]);
    uint64_t m = (uint64_t)(uintptr_t)g_tmeth[k];
    memcpy(g_tdel[k] + 0x28, &m, 8);
    return (uint64_t)(uintptr_t)g_tdel[k];
}
static int regist_action(Cpu *c) {
    if (!g_template && plain_static(c->x[1])) g_template = keep_template(0, c->x[1]);
    return 0;
}
static int regist_function(Cpu *c) {
    if (!g_ftemplate && plain_static(c->x[1])) g_ftemplate = keep_template(1, c->x[1]);
    return 0;
}

// A copy of the template delegate (and its MethodInfo) calling host action i.
static uint64_t make_delegate(int i) {
    uint64_t t = actions[i].function ? g_ftemplate : g_template;
    if (!t) return 0;
    uint8_t *d = calloc(1, 0x80), *m = calloc(1, 0x60);
    if (!d || !m) return 0;
    memcpy(d, (const void *)(uintptr_t)t, 0x68);
    memcpy(m, (const void *)(uintptr_t)ld64(t + 0x28), 0x58);
    uint64_t fn = HOST_ACTIONS + 16 * (uint64_t)i, name = (uint64_t)(uintptr_t)actions[i].name, mp = (uint64_t)(uintptr_t)m;
    memcpy(m + 0x00, &fn, 8);     // methodPointer
    memcpy(m + 0x10, &name, 8);   // name
    memcpy(d + 0x10, &fn, 8);     // method_ptr
    memcpy(d + 0x28, &mp, 8);     // method
    memset(d + 0x48, 0, 16);      // method_info, original_method_info: rebuilt from method on demand
    return (uint64_t)(uintptr_t)d;
}

// EventScript.LoadImpl(this, path): the game's actions are registered; add Cobalt's.
static int load_impl(Cpu *c) {
    uint64_t self = c->x[0], path = c->x[1];   // path: a game string, for String's class
    hook_original(c, "cobalt_eventscript_load", self, path, 0, 0);
    static uint64_t delegates[NACTIONS];
    for (size_t i = 0; i < NACTIONS; i++) {
        if (!delegates[i]) delegates[i] = make_delegate((int)i);
        uint64_t name = cobalt_string(path, actions[i].name);
        if (delegates[i] && name)
            guest_call(c, G(actions[i].function ? REGIST_FUNCTION : REGIST_ACTION), self, delegates[i], name, 0);
    }
    static int logged;
    if (!logged++)
        rt_log("cobalt: lua: %zu functions registered (action template %s, function template %s)", NACTIONS,
               g_template ? (const char *)(uintptr_t)ld64(ld64(g_template + 0x28) + 0x10) : "none",
               g_ftemplate ? (const char *)(uintptr_t)ld64(ld64(g_ftemplate + 0x28) + 0x10) : "none");
    return 1;
}

void cobalt_lua_init(void) {
    hook_install("cobalt_regist_action", regist_action);
    hook_install("cobalt_regist_function", regist_function);
    hook_install("cobalt_eventscript_load", load_impl);
    hook_install("cobalt_script_log", script_log);
    if (nscripts && hook_install("cobalt_map_opening", map_opening))
        rt_log("cobalt: lua: %d script(s) run at the Somniel", nscripts);
}
