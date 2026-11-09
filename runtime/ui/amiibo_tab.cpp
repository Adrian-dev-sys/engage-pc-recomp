// F1 > Amiibo: the dumps in the amiibo folder (TOTK_AMIIBO_DIR, default amiibo/ next to the exe), one row each; A puts
// it on the emulated reader (amiibo.c). Replaces TotK Recomp's two-pane browser with menu rows a gamepad can drive.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <algorithm>
#include <string>
#include <vector>

#include "imgui.h"
#include "ui/amiibo_tab.h"
#include "ui/option_rows.h"

extern "C" {
#include "amiibo.h"
#include "cfg.h"
#include "platform/platform.h"
#include "ui_text.h"
}

static char g_dir[2048];
static std::vector<std::string> g_files;
static uint64_t g_scanned_ns;
static std::string g_failed;   // the last dump that did not load

static void amiibo_dir(void) {
    const char *d = cfg_get("TOTK_AMIIBO_DIR");
    if (d && *d) {
        snprintf(g_dir, sizeof g_dir, "%s", d);
        return;
    }
    char exe[2048];
    g_dir[0] = 0;
    if (!plat_exe_path(exe, sizeof exe)) return;
    char *cut = strrchr(exe, '/'), *back = strrchr(exe, '\\');
    if (back > cut) cut = back;
    if (!cut) return;
    *cut = 0;
    snprintf(g_dir, sizeof g_dir, "%s/amiibo", exe);
}

static int list_cb(const char *name, int kind, uint64_t size, void *ctx) {
    (void)size;
    (void)ctx;
    size_t n = strlen(name);
    if (kind == PLAT_FS_FILE && n > 4 && (!strcasecmp(name + n - 4, ".bin") || !strcasecmp(name + n - 4, ".nfc")))
        g_files.push_back(name);
    return 1;
}

// The folder is listed again every 2 s while the tab shows, so new dumps appear without a button.
static void rescan(void) {
    uint64_t now = plat_now_ns();
    if (g_scanned_ns && now - g_scanned_ns < 2000000000ull) return;
    g_scanned_ns = now;
    amiibo_dir();
    g_files.clear();
    if (g_dir[0] && plat_dir_exists(g_dir)) plat_fs_list(g_dir, list_cb, nullptr);
    std::sort(g_files.begin(), g_files.end());
}

static UiTextId state_text(AmiiboDeviceState s) {
    switch (s) {
    case AMIIBO_DEV_STATE_SEARCHING: return UI_MENU_AMIIBO_STATE_SEARCHING;
    case AMIIBO_DEV_STATE_TAG_FOUND: return UI_MENU_AMIIBO_STATE_FOUND;
    case AMIIBO_DEV_STATE_TAG_MOUNTED: return UI_MENU_AMIIBO_STATE_MOUNTED;
    case AMIIBO_DEV_STATE_UNAVAILABLE: return UI_MENU_AMIIBO_STATE_OFF;
    default: return UI_MENU_AMIIBO_STATE_IDLE;
    }
}

extern "C" void amiibo_frame(void) {
    amiibo_set_auto_remove(cfg_double("TOTK_AMIIBO_AUTO_LIFT") != 0);
    amiibo_tick();
}

void tab_amiibo(float scale) {
    rescan();
    opt_begin("##amiibo", scale);
    opt_group(UI_MENU_AMIIBO_READER);
    const AmiiboInfo *info = amiibo_has_tag() ? amiibo_get_current_info() : nullptr;
    opt_text_row("##amiibo_state", menu_text(UI_MENU_AMIIBO_STATE), menu_text(state_text(amiibo_get_reader_state())),
                 info ? info->filename : nullptr, menu_text(UI_MENU_AMIIBO_STATE_DESC), false);
    if (opt_button("##amiibo_lift", UI_MENU_AMIIBO_REMOVE, UI_MENU_AMIIBO_REMOVE_DESC)) amiibo_remove_tag();
    opt_toggle("TOTK_AMIIBO_AUTO_LIFT", UI_MENU_AMIIBO_AUTO_REMOVE, UI_MENU_AMIIBO_AUTO_REMOVE_DESC);

    opt_group(UI_MENU_AMIIBO_FILES);
    if (g_files.empty()) opt_text(UI_MENU_AMIIBO_NONE);
    for (size_t i = 0; i < g_files.size(); i++) {
        const std::string &f = g_files[i];
        char id[32];
        snprintf(id, sizeof id, "##amiibo%zu", i);
        bool on = info && !strcmp(info->filename, f.c_str());
        const char *note = f == g_failed ? menu_text(UI_MENU_AMIIBO_BAD) : nullptr;
        int s = opt_text_row(id, f.c_str(), menu_text(on ? UI_MENU_AMIIBO_ON_READER : UI_MENU_AMIIBO_TAP), note,
                             menu_text(UI_MENU_AMIIBO_ROW_DESC), false);
        if (s == 1 || s == 2 || s == -1) {
            std::string path = std::string(g_dir) + "/" + f;
            if (amiibo_scan_and_tap(path.c_str())) g_failed.clear();
            else g_failed = f;
        }
    }
    static uint64_t open_failed;
    if (opt_button("##amiibo_open", UI_MENU_AMIIBO_OPEN, UI_MENU_AMIIBO_OPEN_DESC,
                   plat_now_ns() < open_failed ? menu_text(UI_MENU_ACTION_FAILED) : "")) {
        amiibo_dir();
        bool ok = g_dir[0] != 0;
        if (ok) plat_file_mkdir_p(g_dir);
        if (!ok || !plat_open_folder(g_dir)) open_failed = plat_now_ns() + 3000000000ull;
        g_scanned_ns = 0;
    }
    opt_end();
}
