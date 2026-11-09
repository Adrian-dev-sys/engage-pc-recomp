// Cobalt mods' root-level Lua scripts (the behaviour of Cobalt's script.rs): each <mod>/<Name>.lua runs when a map
// opens in the Somniel (the hub). Its code is run in the event script's Lua state, then its function <Name> (the file
// name, same case) is started as a coroutine of the opening sequence. Hook cobalt_map_opening on
// App.EventSequence::MapOpening(parent).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cobalt_lua.h"
#include "cobalt_voice.h"
#include "hooks.h"
#include "host.h"
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

void cobalt_lua_init(void) {
    hook_install("cobalt_script_log", script_log);
    if (nscripts && hook_install("cobalt_map_opening", map_opening))
        rt_log("cobalt: lua: %d script(s) run at the Somniel", nscripts);
}
