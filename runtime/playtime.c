// Adapted from TotK Recomp runtime/playtime.c (MIT, Copyright (c) 2026 Valiantsin Dzerakh and contributors), without
// its probe.
// Play time: how long the player has spent in the game, across runs, shown on the Game tab of the settings menu
// (ui/menu.cpp tab_game). The game itself keeps no play time a player can see, and the Switch's own play log is not
// there to read, so the port counts it.
//
// One host thread (it never holds a virtual core, as discord_rpc.c's) wakes every second and adds the time since its
// last wake when the window has the focus (gfx_window_focused) and the game presented at least one frame meanwhile
// (gfx_stats). So the shader prewarm before the first frame, a window left in the background and a hung game do not
// count; loading screens and the game's own menus do, as they present frames. One wake adds at most TICK_CAP_MS, so a
// machine that slept with the game open adds two seconds, not the night.
//
// The total lives in <profile>/playtime.txt, next to account.uid and the saves, so it stays with them when the
// install is updated or moved. It is plain text, "total_seconds <n>", written atomically (plat_file_write_atomic)
// every SAVE_EVERY_MS while it grows and once more when the game exits (playtime_save from boot.c, svc.c and
// gfx_present.c, next to prof_write); a crash loses at most SAVE_EVERY_MS. A file that does not read as that is kept
// as playtime.txt.bad and the count starts again from zero. A scripted run (--seconds) reads the total and adds nothing.
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "host.h"
#include "platform/platform.h"

#define TICK_MS 1000
#define TICK_CAP_MS 2000
#define SAVE_EVERY_MS 30000
#define TOTAL_CAP_S (100ull * 365 * 24 * 3600)   // a hundred years: anything larger is a damaged file

static PlatMutex lock = PLAT_MUTEX_INIT;   // the totals and the file
static uint64_t total_ms, session_ms, saved_ms;
static int loaded;                         // 1 once the file was read (or found missing), -1 when there is no profile
static char path[1100];

// The seconds of a playtime.txt: "total_seconds <n>", a line end or nothing after it, a UTF-8 byte order mark before it
// (an editor may add one). 1 when the file reads so and n is at most TOTAL_CAP_S.
static int parse(const uint8_t *b, uint64_t n, uint64_t *seconds) {
    if (n >= 3 && !memcmp(b, "\xEF\xBB\xBF", 3)) b += 3, n -= 3;
    char text[64] = {0};
    memcpy(text, b, n < sizeof text - 1 ? n : sizeof text - 1);
    unsigned long long s = 0;
    char tail = 0;
    if (sscanf(text, "total_seconds %llu%c", &s, &tail) < 1 || (tail && tail != '\n' && tail != '\r') ||
        s > TOTAL_CAP_S)
        return 0;
    *seconds = s;
    return 1;
}

// Holding lock. dir: the profile directory.
static void load(const char *dir) {
    if (loaded) return;
    if (!dir || !*dir || snprintf(path, sizeof path, "%s/playtime.txt", dir) >= (int)sizeof path) {
        loaded = -1;
        rt_log("playtime: no profile directory; play time is not counted");
        return;
    }
    loaded = 1;
    uint64_t n = 0, s = 0;
    uint8_t *b = plat_file_read(path, &n);
    if (!b) {
        rt_log("playtime: no %s yet; counting from zero", path);
        return;
    }
    if (parse(b, n, &s)) {
        free(b);
        total_ms = saved_ms = s * 1000;
        rt_log("playtime: %llu h %02llu min played before this run (%s)", (unsigned long long)(s / 3600),
               (unsigned long long)(s / 60 % 60), path);
        return;
    }
    char bad[1110];
    snprintf(bad, sizeof bad, "%s.bad", path);
    int kept = plat_file_write_atomic(bad, b, (size_t)n);   // the whole file, for the player to recover the number
    free(b);
    rt_log("playtime: %s does not read as \"total_seconds <n>\"; %s, counting from zero", path,
           kept ? "kept as playtime.txt.bad" : "could not keep a copy");
}

// Holding lock.
static void save_locked(void) {
    if (loaded != 1 || total_ms / 1000 == saved_ms / 1000) return;
    char text[48];
    int n = snprintf(text, sizeof text, "total_seconds %" PRIu64 "\n", total_ms / 1000);
    if (plat_file_write_atomic(path, text, (size_t)n)) saved_ms = total_ms;
    else rt_log("playtime: cannot write %s", path);
}

void playtime_save(void) {
    plat_mutex_lock(&lock);
    save_locked();
    plat_mutex_unlock(&lock);
}

int playtime_get(uint64_t *total_s, uint64_t *session_s) {
    plat_mutex_lock(&lock);
    int ok = loaded == 1;
    *total_s = total_ms / 1000;
    *session_s = session_ms / 1000;
    plat_mutex_unlock(&lock);
    return ok;
}

// Holding lock: the time since the last tick, d ms, when the game was played meanwhile; the file every SAVE_EVERY_MS.
static void tick_locked(uint64_t now, uint64_t d, int playing, uint64_t *last_save) {
    if (d > TICK_CAP_MS) d = TICK_CAP_MS;
    if (playing) total_ms += d, session_ms += d;
    if (now - *last_save >= SAVE_EVERY_MS) save_locked(), *last_save = now;
}

static void playtime_main(void *arg) {
    (void)arg;
    plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
    uint64_t last = plat_now_ms(), last_save = last, frames = gfx_stats().frames;
    for (;;) {
        plat_sleep_ms(TICK_MS);
        uint64_t now = plat_now_ms(), f = gfx_stats().frames;
        int playing = f != frames && gfx_window_focused();
        plat_mutex_lock(&lock);
        tick_locked(now, now - last, playing, &last_save);
        plat_mutex_unlock(&lock);
        last = now, frames = f;
    }
}

// The file is read at once, so the Game tab shows the total from the first frame. A scripted run (--seconds: benches,
// menu shots, input scripts) shows it but adds nothing: those runs are not play.
void playtime_init(void) {
    static int32_t started;
    if (plat_xchg32(&started, 1)) return;
    plat_mutex_lock(&lock);
    load(rt_config()->profile);
    int on = loaded == 1;
    plat_mutex_unlock(&lock);
    if (!on) return;
    if (rt_config()->seconds > 0) {
        rt_log("playtime: a scripted run (--seconds); play time is shown, not counted");
        return;
    }
    if (!plat_thread_spawn(playtime_main, NULL, "playtime", 0))
        rt_log("playtime: cannot start its thread; play time is not counted");
}
