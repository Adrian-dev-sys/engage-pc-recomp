// Cobalt data mods without Cobalt (cobalt.c): at start, the enabled Cobalt-layout mods' new asset bundles, XML
// gamedata patches and MSBT text patches are merged into the game's catalog, gamedata and message bundles, as Cobalt
// does on the Switch. The merged bundles are cached under <profile>/cobalt and served on top of every mod.
#pragma once

void cobalt_bake(void);   // romfs.c overlay_init, after the mods' overlays are listed and before their contest
