// Host hooks on guest functions (hooks.c): a `hook <name> <module> <offset>` line of the patch file makes the lifted
// function call rt_hook_enter at its start. The installed HookFn sees the guest arguments in c->x[0..7]; it returns 1
// when it handled the call (its result in c->x[0]) and 0 to let the guest function run.
#pragma once
#include <stdint.h>

#include "cpu.h"

typedef int (*HookFn)(Cpu *c);

int hook_install(const char *name, HookFn fn);   // before the game starts; 0 when the build has no such hook
uint64_t hook_address(const char *name);         // the hooked function's guest address, 0 if none
// Calls the hooked guest function itself (without the hook) with up to 8 arguments; returns x0. The Cpu's other
// registers are kept.
uint64_t hook_original(Cpu *c, const char *name, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
uint64_t hook_main(uint64_t offset);               // main's guest address of a module offset
// Calls the guest function at addr with up to 4 arguments from a hook; returns x0, the Cpu's registers kept.
uint64_t guest_call(Cpu *c, uint64_t addr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
uint64_t guest_call6(Cpu *c, uint64_t addr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
