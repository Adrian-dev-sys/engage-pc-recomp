// Cobalt's voice handling for mods' voice banks (cobalt_voice.c).
#pragma once

void cobalt_voice_init(void);   // cobalt.c, when a Cobalt mod adds sound banks
#include <stdint.h>
// A System.String of text in host memory, kept for the run. a_string: any game string (for its class), or 0 to reuse
// the class of one seen before.
uint64_t cobalt_string(uint64_t a_string, const char *text);
