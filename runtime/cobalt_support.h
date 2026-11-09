// Cobalt's additional supports from mods' patches/xml/Support.xml (cobalt_support.c).
#pragma once

void cobalt_support_add_file(const char *path);   // cobalt.c, in merge order
void cobalt_support_init(void);                    // installs the hook when a mod adds supports
