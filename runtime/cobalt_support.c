// Cobalt's additional supports (the behaviour of Cobalt's support.rs): patches/xml/Support.xml of each mod lists, per
// character (a row with Condition="<PID>"), the characters it supports (the following rows' Pid) and their
// experience curve (ExpType, an index into the game's RelianceExpData list). The game has no such sheet, so the rows
// are read here; a later mod's row for the same pair wins. Hook cobalt_reliance_exp on App.RelianceData::TryGetExp.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cobalt_support.h"
#include "hooks.h"
#include "host.h"
#include "platform/platform.h"

// main offsets (tools/il2cpp_map.py, tools/il2cpp_usages.py)
#define RELIANCE_GET_PID 0x200f7c0      // RelianceData.get_Pid
#define SLOT_RELIANCE_TRYGET 0x617e450  // StructData<RelianceData>.TryGet(int) static
#define SLOT_REXP_TRYGET 0x617e470      // StructData<RelianceExpData>.TryGet(int) static
#define INIT_METADATA 0x492060
#define OBJECT_NEW 0x4921f0             // il2cpp object allocation (class)
#define REXP_CTOR 0x2010b40             // RelianceExpData..ctor
#define REXP_GET_REXID 0x2010ab0        // RelianceExpData get_Rexid / set_Rexid(string)
#define REXP_SET_REXID 0x2010ac0
#define REXP_GET_C 0x2010ad0            // get_ExpC / set_ExpC(byte), likewise B and A
#define REXP_SET_C 0x2010ae0
#define REXP_GET_B 0x2010af0
#define REXP_SET_B 0x2010b00
#define REXP_GET_A 0x2010b10
#define REXP_SET_A 0x2010b20

typedef struct { char cond[128], pid[128]; int exp; } Row;   // exp < 0: none
static Row *rows;
static int nrows, caprows;

static void put(const char *cond, const char *pid, int exp) {
    for (int i = 0; i < nrows; i++)
        if (!strcmp(rows[i].cond, cond) && !strcmp(rows[i].pid, pid)) {
            rows[i].exp = exp;
            return;
        }
    if (nrows == caprows) {
        caprows = caprows ? caprows * 2 : 64;
        Row *r = realloc(rows, sizeof *rows * (size_t)caprows);
        if (!r) return;
        rows = r;
    }
    snprintf(rows[nrows].cond, sizeof rows->cond, "%s", cond);
    snprintf(rows[nrows].pid, sizeof rows->pid, "%s", pid);
    rows[nrows++].exp = exp;
}

// The value of attribute name in the tag [p, end), "" when it is absent.
static void attr(const char *p, const char *end, const char *name, char *out, size_t cap) {
    out[0] = 0;
    size_t n = strlen(name);
    for (const char *q = p; q + n + 2 < end; q++)
        if (!memcmp(q, name, n) && q[n] == '=' && q[n + 1] == '"' && (q == p || q[-1] == ' ' || q[-1] == '\t')) {
            const char *v = q + n + 2, *e = memchr(v, '"', (size_t)(end - v));
            if (e) snprintf(out, cap, "%.*s", (int)(e - v), v);
            return;
        }
}

void cobalt_support_add_file(const char *path) {
    uint64_t n = 0;
    char *t = (char *)plat_file_read(path, &n);
    if (!t) return;
    const char *p = t, *end = t + n, *data = strstr(t, "<Data>");
    if (data) p = data;
    char group[128] = "";
    int before = nrows;
    while ((p = strstr(p, "<Param")) && p < end) {
        const char *e = strchr(p, '>');
        if (!e) break;
        char cond[128], pid[128], exp[16];
        attr(p, e, "Condition", cond, sizeof cond);
        attr(p, e, "Pid", pid, sizeof pid);
        attr(p, e, "ExpType", exp, sizeof exp);
        if (cond[0]) snprintf(group, sizeof group, "%s", cond);   // a new character's group
        if (group[0] && pid[0]) put(group, pid, exp[0] ? atoi(exp) : -1);
        p = e + 1;
    }
    free(t);
    rt_log("cobalt: support: %s: %d pair(s)", path, nrows - before);
}

// A game string (UTF-16) as UTF-8.
static void utf8(uint64_t s, char *out, size_t cap) {
    out[0] = 0;
    if (!s) return;
    int32_t len = *(const int32_t *)(uintptr_t)(s + 0x10);
    const uint16_t *w = (const uint16_t *)(uintptr_t)(s + 0x14);
    size_t k = 0;
    for (int32_t q = 0; q < len && k + 5 < cap; q++) {
        uint32_t ch = w[q];
        if (ch >= 0xD800 && ch < 0xDC00 && q + 1 < len) ch = 0x10000 + ((ch - 0xD800) << 10) + (w[++q] - 0xDC00u);
        if (ch < 0x80) {
            out[k++] = (char)ch;
        } else if (ch < 0x800) {
            out[k++] = (char)(0xC0 | ch >> 6), out[k++] = (char)(0x80 | (ch & 0x3F));
        } else if (ch < 0x10000) {
            out[k++] = (char)(0xE0 | ch >> 12), out[k++] = (char)(0x80 | (ch >> 6 & 0x3F));
            out[k++] = (char)(0x80 | (ch & 0x3F));
        } else {
            out[k++] = (char)(0xF0 | ch >> 18), out[k++] = (char)(0x80 | (ch >> 12 & 0x3F));
            out[k++] = (char)(0x80 | (ch >> 6 & 0x3F)), out[k++] = (char)(0x80 | (ch & 0x3F));
        }
    }
    out[k] = 0;
}

static uint64_t slot(Cpu *c, uint64_t off) {
    guest_call(c, hook_main(INIT_METADATA), hook_main(off), 0, 0, 0);
    return *(const uint64_t *)(uintptr_t)hook_main(off);
}

// RelianceData.TryGetExp(this, index) -> RelianceExpData: this character's support curve with RelianceData[index]
static int try_get_exp(Cpu *c) {
    uint64_t self = c->x[0];
    uint32_t index = (uint32_t)c->x[1];
    char me[128], other[128];
    utf8(guest_call(c, hook_main(RELIANCE_GET_PID), self, 0, 0, 0), me, sizeof me);
    int any = 0;
    for (int i = 0; i < nrows && !any; i++) any = !strcmp(rows[i].cond, me);
    if (!any) return 0;
    uint64_t get = slot(c, SLOT_RELIANCE_TRYGET);
    uint64_t rd = get ? guest_call(c, *(const uint64_t *)(uintptr_t)get, index, get, 0, 0) : 0;
    if (!rd) return 0;
    utf8(guest_call(c, hook_main(RELIANCE_GET_PID), rd, 0, 0, 0), other, sizeof other);
    for (int i = 0; i < nrows; i++) {
        if (strcmp(rows[i].cond, me) || strcmp(rows[i].pid, other) || rows[i].exp < 0) continue;
        uint64_t gx = slot(c, SLOT_REXP_TRYGET);
        uint64_t exp = gx ? guest_call(c, *(const uint64_t *)(uintptr_t)gx, (uint32_t)rows[i].exp, gx, 0, 0) : 0;
        if (!exp) return 0;
        static volatile int32_t n;
        if (plat_inc32(&n) <= 20) rt_log("cobalt: support: %s with %s uses curve %d", me, other, rows[i].exp);
        // A new RelianceExpData with the curve's four values, as Cobalt returns (not the shared list entry).
        uint64_t copy = guest_call(c, hook_main(OBJECT_NEW), *(const uint64_t *)(uintptr_t)exp, 0, 0, 0);
        if (!copy) return 0;
        guest_call(c, hook_main(REXP_CTOR), copy, 0, 0, 0);
        guest_call(c, hook_main(REXP_SET_REXID), copy, guest_call(c, hook_main(REXP_GET_REXID), exp, 0, 0, 0), 0, 0);
        guest_call(c, hook_main(REXP_SET_C), copy, guest_call(c, hook_main(REXP_GET_C), exp, 0, 0, 0) & 0xFF, 0, 0);
        guest_call(c, hook_main(REXP_SET_B), copy, guest_call(c, hook_main(REXP_GET_B), exp, 0, 0, 0) & 0xFF, 0, 0);
        guest_call(c, hook_main(REXP_SET_A), copy, guest_call(c, hook_main(REXP_GET_A), exp, 0, 0, 0) & 0xFF, 0, 0);
        c->x[0] = copy;
        return 1;
    }
    return 0;
}

void cobalt_support_init(void) {
    if (nrows && hook_install("cobalt_reliance_exp", try_get_exp))
        rt_log("cobalt: support: %d additional support pair(s)", nrows);
}
