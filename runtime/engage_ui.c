// Engage UI fixes on a screen wider than 16:9 (set uwui): objects a scene places at a fixed spot of the 1920-wide UI.
#include <string.h>

#include "hooks.h"
#include "host.h"

#define RD(a) (*(const uint64_t *)(uintptr_t)(a))
#define G(off) hook_main(off)

// main offsets (build/engage-v200/il2cpp-methods.tsv)
#define GO_TRANSFORM 0x2c4e880       // GameObject.get_transform
#define TR_PARENT 0x37909f0          // Transform.get_parent
#define TR_GET_POS 0x378f890         // Transform.get_position
#define TR_SET_POS 0x378f940         // Transform.set_position(Vector3)
#define OBJ_NAME 0x32e8e90           // Object.get_name

static void get_v3(Cpu *c, float v[3]) {
    for (int i = 0; i < 3; i++) memcpy(&v[i], &c->v[i], 4);
}
static void set_v3(Cpu *c, const float v[3]) {
    for (int i = 0; i < 3; i++) {
        uint32_t b;
        memcpy(&b, &v[i], 4);
        c->v[i] = b;
    }
}

static void name_of(Cpu *c, uint64_t obj, char *out, size_t cap) {
    out[0] = 0;
    uint64_t s = obj ? guest_call(c, G(OBJ_NAME), obj, 0, 0, 0) : 0;
    if (!s) return;
    int32_t n = *(const int32_t *)(uintptr_t)(s + 0x10);
    const uint16_t *w = (const uint16_t *)(uintptr_t)(s + 0x14);
    size_t k = 0;
    for (int32_t i = 0; i < n && k + 1 < cap; i++) out[k++] = w[i] < 0x80 ? (char)w[i] : '?';
    out[k] = 0;
}

// DragonRideCamera.InitializeCameraBase: Wyvern Ride's reticle (m_CursorObj, +0x88, "Cursor" under "SightCursor" in
// the screen-space DragonRideCanvas) is laid out for 16:9: it sits the side margin left of the wider screen's centre,
// where the shots go. Its parent moves right by that margin (screen pixels, patch page +0x5c), so whatever moves the
// reticle inside it still works.
static int ride_camera_init(Cpu *c) {
    uint64_t self = c->x[0];
    uint64_t r = hook_original(c, "uw_ride_camera_init", self, 0, 0, 0);
    float margin = rt_patch_page() ? *(const float *)((const uint8_t *)rt_patch_page() + 0x5c) : 0;
    uint64_t obj = margin > 0 ? RD(self + 0x88) : 0;
    uint64_t t = obj ? guest_call(c, G(GO_TRANSFORM), obj, 0, 0, 0) : 0;
    uint64_t parent = t ? guest_call(c, G(TR_PARENT), t, 0, 0, 0) : 0;
    if (parent) {
        float p[3];
        guest_call(c, G(TR_GET_POS), parent, 0, 0, 0);
        get_v3(c, p);
        float was = p[0];
        p[0] += margin;
        set_v3(c, p);
        guest_call(c, G(TR_SET_POS), parent, 0, 0, 0);
        char nm[64];
        name_of(c, parent, nm, sizeof nm);
        rt_log("ui: Wyvern Ride reticle parent %s moved right by %.0f px (x %.0f -> %.0f)", nm, (double)margin,
               (double)was, (double)p[0]);
    }
    c->x[0] = r;
    return 1;
}

void engage_ui_init(void) {
    if (!RT_ENGAGE) return;
    hook_install("uw_ride_camera_init", ride_camera_init);
}
