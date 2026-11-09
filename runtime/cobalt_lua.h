// Cobalt mods' root-level Lua scripts, run at the Somniel (cobalt_lua.c).
#pragma once

void cobalt_lua_add(const char *file, const char *name);   // cobalt.c: <mod>/<name>.lua, in merge order
void cobalt_lua_init(void);                                 // installs the hook when there is a script
