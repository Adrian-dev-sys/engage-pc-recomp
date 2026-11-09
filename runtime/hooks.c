// Host hooks on guest functions (hooks.h). recomp/patches.py `hook` lines list them; recomp/gen.py starts each
// hooked function with `if (rt_hook_on[k] && rt_hook_enter(c, k)) return;`.
#include <string.h>

#include "host.h"
#include "hooks.h"
#include "layout.h"
#include "patches.h"
#include "platform/platform.h"

#ifndef RT_HOOK_COUNT
#define RT_HOOK_COUNT 0
#define RT_HOOKS(X)
#endif

uint8_t rt_hook_on[RT_HOOK_COUNT ? RT_HOOK_COUNT : 1];
static HookFn fns[RT_HOOK_COUNT ? RT_HOOK_COUNT : 1];

typedef struct { const char *name, *module; uint64_t offset; } Row;
#define ROW(i, n, m, o) {#n, #m, o},
static const Row rows[RT_HOOK_COUNT ? RT_HOOK_COUNT : 1] = {RT_HOOKS(ROW)};
#undef ROW

#define BASE(lo, up) {#lo, MOD_##up##_BASE},
static const struct { const char *name; uint64_t base; } bases[] = {MOD_LIST(BASE)};
#undef BASE

static int index_of(const char *name) {
    for (int i = 0; i < RT_HOOK_COUNT; i++)
        if (rows[i].name && !strcmp(rows[i].name, name)) return i;
    return -1;
}

uint64_t hook_address(const char *name) {
    int k = index_of(name);
    if (k < 0) return 0;
    for (size_t i = 0; i < sizeof bases / sizeof *bases; i++)
        if (!strcmp(bases[i].name, rows[k].module)) return bases[i].base + rows[k].offset;
    return 0;
}

int hook_install(const char *name, HookFn fn) {
    int k = index_of(name);
    if (k < 0) {
        rt_log("hooks: this build has no hook %s (recomp/patches-*.txt)", name);
        return 0;
    }
    fns[k] = fn;
    rt_hook_on[k] = fn != NULL;
    return 1;
}

// The guest threads running the original of a hook: the next entry of hook k on that Cpu skips the hook once.
static PlatMutex lock = PLAT_MUTEX_INIT;
static struct { Cpu *c; int k; } skips[64];

static int take_skip(Cpu *c, int k) {
    int hit = 0;
    plat_mutex_lock(&lock);
    for (size_t i = 0; i < sizeof skips / sizeof *skips; i++)
        if (skips[i].c == c && skips[i].k == k) {
            skips[i].c = NULL, hit = 1;
            break;
        }
    plat_mutex_unlock(&lock);
    return hit;
}

int rt_hook_enter(Cpu *c, int k) {
    if (take_skip(c, k)) return 0;
    return fns[k] ? fns[k](c) : 0;
}

uint64_t hook_original(Cpu *c, const char *name, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    int k = index_of(name);
    uint64_t addr = hook_address(name);
    if (k < 0 || !addr) rt_fatal("hooks: hook_original(%s): no such hook", name);
    plat_mutex_lock(&lock);
    size_t i = 0;
    while (i < sizeof skips / sizeof *skips && skips[i].c) i++;
    if (i == sizeof skips / sizeof *skips) rt_fatal("hooks: more than 64 nested originals");
    skips[i].c = c, skips[i].k = k;
    plat_mutex_unlock(&lock);
    return guest_call(c, addr, a0, a1, a2, a3);
}

uint64_t hook_main(uint64_t offset) { return MOD_MAIN_BASE + offset; }

uint64_t guest_call(Cpu *c, uint64_t addr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    return guest_call6(c, addr, a0, a1, a2, a3, 0, 0);
}

uint64_t guest_call6(Cpu *c, uint64_t addr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    uint64_t keep[19], lr = c->x[30];   // x0..x18: the hook's caller may still need them
    memcpy(keep, c->x, sizeof keep);
    uint64_t r = rt_call(c, addr, 6, a0, a1, a2, a3, a4, a5);
    memcpy(c->x, keep, sizeof keep);
    c->x[30] = lr;
    return r;
}
