// Log the first managed/native exceptions without changing the guest's unwind ABI.
#include <string.h>
#include "host.h"
#include "kernel.h"
#include "platform/platform.h"

static void describe_managed_exception(uint64_t wrapper) {
    uint64_t object = 0, fields[24] = {0}, klass[4] = {0};
    if (!plat_mem_read(wrapper, &object, sizeof object) || !plat_mem_read(object, fields, sizeof fields) ||
        !plat_mem_read(fields[0], klass, sizeof klass)) return;
    char type[256];
    if (plat_mem_read(klass[2], type, sizeof type)) {
        type[sizeof type - 1] = 0;
        rt_log("exception: managed type %s", type);
    }
    for (unsigned i = 2; i < 24; i++) {
        uint64_t candidate[3] = {0}, string_class[4] = {0};
        char name[7] = {0};
        if (!fields[i] || !plat_mem_read(fields[i], candidate, sizeof candidate) ||
            !plat_mem_read(candidate[0], string_class, sizeof string_class) ||
            !plat_mem_read(string_class[2], name, sizeof name) || strcmp(name, "String")) continue;
        int32_t length = (int32_t)candidate[2];
        if (length <= 0 || length > 1024) continue;
        uint16_t chars[1024]; char message[1025];
        if (!plat_mem_read(fields[i] + 20, chars, (size_t)length * 2)) continue;
        for (int j = 0; j < length; j++) message[j] = chars[j] < 128 ? (char)chars[j] : '?';
        message[length] = 0;
        rt_log("exception: managed string field +%#x: %s", i * 8, message);
    }
}

static void observe_throw(Cpu *c) {
    static volatile int32_t throws;
    if (RT_ENGAGE && plat_inc32(&throws) <= 8) {
        uint64_t type[2] = {0};
        char name[256] = "(unreadable)", caller[160];
        if (plat_mem_read(c->x[1], type, sizeof type) && type[1]) {
            if (!plat_mem_read(type[1], name, sizeof name)) strcpy(name, "(unreadable)");
            name[sizeof name - 1] = 0;
        }
        rt_log("exception: throw %s object %#llx from %s", name, (unsigned long long)c->x[0],
               ldr_describe(c->x[30], caller, sizeof caller));
        if (!strcmp(name, "22Il2CppExceptionWrapper")) describe_managed_exception(c->x[0]);
        kern_backtrace(c);
    }
    // rt_original uses a synthetic LR. C++ unwinding instead needs the real guest caller.
    GuestFn original = rt_dispatch_lookup(ldr_symbol("__cxa_throw"));
    if (!original) rt_fatal("exception: no guest __cxa_throw");
    original(c);
    rt_fatal("exception: guest __cxa_throw returned");
}

HOST_OBSERVER("__cxa_throw", observe_throw);

static void observe_raise(Cpu *c) {
    uint64_t stack = c->sp;
    GuestFn original = rt_dispatch_lookup(ldr_symbol("_Unwind_RaiseException"));
    if (!original) rt_fatal("exception: no guest _Unwind_RaiseException");
    original(c);
    rt_log("exception: _Unwind_RaiseException returned %#llx, SP %#llx -> %#llx",
           (unsigned long long)c->x[0], (unsigned long long)stack, (unsigned long long)c->sp);
}

HOST_OBSERVER("_Unwind_RaiseException", observe_raise);
