// Cobalt's extras: combat switches, ring polish rumble, game mode, engage zone remapping (cobalt_extras.c).
#pragma once
#include "cpu.h"

void cobalt_extras_init(void);            // installs the hooks (cobalt.c)
void cobalt_extras_capture(Cpu *c);       // notes the GameUserData singleton, from any hook
int cobalt_game_mode(void);               // the loaded save's mode (0 casual, 1 classic, 2 phoenix), -1 unknown
void cobalt_set_game_mode(int mode);      // F1 > Mods
