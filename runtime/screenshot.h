// F12 screenshots (screenshot.c): PNGs of the presented frame in screenshots/ next to the exe.
#pragma once
#include <stdint.h>

void screenshot_request(void);   // any thread: take one at the next present
int screenshot_claim(void);      // present thread: 1 once per request
// Present thread: copies px (w x h, 4 bytes a pixel; bgra: B,G,R,A order) and writes the PNG on a worker thread.
void screenshot_take(const uint8_t *px, int w, int h, int bgra);
