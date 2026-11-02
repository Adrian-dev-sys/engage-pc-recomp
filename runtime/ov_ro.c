// Engage imports nn::ro but ships no NRO files. The host loader owns its NSO symbol table;
// connecting the SDK to an absent ldr:ro service would dereference its null session.
#include "host.h"

static void ov_ro_initialize(Cpu *c) { ret_x(c, 0); }

static void ov_ro_lookup(Cpu *c) {
    uint64_t *out = arg_p(c, 0);
    const char *name = arg_p(c, 1);
    uint64_t symbol = name ? ldr_symbol(name) : 0;
    if (out && symbol) *out = symbol;
    ret_x(c, out && symbol ? 0 : RT_RESULT(22, 1));
}

HOST_OVERRIDE("_ZN2nn2ro10InitializeEv", ov_ro_initialize);
HOST_OVERRIDE("_ZN2nn2ro12LookupSymbolEPmPKc", ov_ro_lookup);
