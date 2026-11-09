// Player mods (mods.c): one folder per mod under mods/ next to engage.exe (TOTK_MODS_DIR), unpacked as emulators take
// them: <mod>/romfs, or the Atmosphere layout <mod>/atmosphere/contents/<title id>/romfs. Enabled mods' RomFS files
// replace or add game files (romfs.c); the top mod wins a file two mods give. ExeFS patches (.ips, .pchtxt) and code
// modules cannot be applied to the recompiled code: such a mod is listed and stays off. The list is scanned once at
// start; its order and switches live in engage.ini [mods] (TOTK_MODS_ORDER: names top first, separated by '|';
// TOTK_MODS_DISABLED: names switched off).
// Adapted from TotK Recomp runtime/mods.h (MIT, Copyright (c) 2026 Valiantsin Dzerakh and contributors).
#pragma once
#include <stdint.h>

// ModInfo.kind bits. MOD_COBALT: a Cobalt mod (config.yaml / Data / patches at its root, which mirrors the RomFS;
// cobalt.c merges its patches). MOD_NRO: a Cobalt code plugin (.nro) at its root, which cannot run here.
enum { MOD_ROMFS = 1, MOD_EXEFS = 2, MOD_SUBSDK = 4, MOD_CHEATS = 8, MOD_COBALT = 16, MOD_NRO = 32 };
typedef enum {
    MOD_OK,
    MOD_EMPTY,        // no RomFS file
    MOD_CODE,         // has ExeFS patches or code modules: not applicable to the recompiled game, the whole mod stays off
    MOD_BAD_NAME,     // its folder name holds '|', the separator of the order keys
} ModProblem;

typedef struct {
    char name[256];        // folder name, UTF-8
    char romfs[2048];      // UTF-8 path of its RomFS root; "" none
    char exefs[2048];      // UTF-8 path of its ExeFS folder; "" none
    int kind, enabled;
    ModProblem problem;
    uint32_t replaced, added, overridden;   // romfs.c: files served (replacing / added), files a higher mod serves
    char overridden_by[256];                // the first higher mod that took one of its files
} ModInfo;

void mods_scan(void);              // once; later calls return at once
const char *mods_dir(void);        // the folder scanned (default: <dir of engage.exe>/mods)
int mods_count(void);
ModInfo *mods_at(int i);           // 0 = top of the order; NULL out of range
// Menu edits: change the list and write TOTK_MODS_ORDER / TOTK_MODS_DISABLED; they apply at the next start.
void mods_set_enabled(int i, int on);
void mods_move(int i, int delta);  // -1 up, +1 down; nothing at the ends
int mods_changed(void);            // 1 when the order or a switch differs from the start
