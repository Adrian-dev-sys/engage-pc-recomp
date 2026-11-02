import os
import struct
import subprocess
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'recomp'), str(ROOT / 'tools')]
import eh
import gen
from lifter import Lifter


class ExceptionTablesTest(unittest.TestCase):
    def test_call_sites_and_zero_landing(self):
        # LPStart omitted, types omitted, ULEB call-site fields, two entries.
        image = bytes([255, 255, 1, 8, 0, 4, 16, 1, 4, 4, 0, 0])
        self.assertEqual(eh.lsda_pads(image, 0, 0x1000), {0x1010})
        with self.assertRaises(ValueError):
            eh.lsda_pads(image[:-1], 0, 0x1000)

    def test_cie_fde_and_lsda_chain(self):
        image = bytearray(512)
        image[16:36] = bytes([1, 0x1b, 3, 0x3b]) + struct.pack('<iIii', 20, 1, 0x100 - 16, 72 - 16)
        cie = struct.pack('<I', 0) + bytes([1]) + b'zLR\0' + bytes([1, 0x78, 30, 2, 0x1b, 0x1b])
        image[40:44 + len(cie)] = struct.pack('<I', len(cie)) + cie
        fde = struct.pack('<IiiB', 76 - 40, 0x100 - 80, 0x20, 4) + struct.pack('<i', 128 - 89)
        image[72:76 + len(fde)] = struct.pack('<I', len(fde)) + fde
        image[128:136] = bytes([255, 255, 1, 4, 0, 4, 16, 1])
        mod = SimpleNamespace(image=image, nso=SimpleNamespace(eh_start=16), base=0x7000000000,
                              in_text=lambda a: 0x7000000100 <= a < 0x7000000120 and a % 4 == 0)
        self.assertEqual(eh.landing_pads(mod), {0x7000000110})

    def test_encoded_relative_signed_and_bounds(self):
        self.assertEqual(eh.Reader(b'1234' + struct.pack('<i', -4), 4).encoded(0x1b), 0)
        with self.assertRaises(ValueError):
            eh.Reader(b'\x80' * 10, 0).leb()
        with self.assertRaises(ValueError):
            eh.Reader(b'\0', 0).integer(4)


class ContinuationTest(unittest.TestCase):
    def test_generated_nonlocal_return_skips_old_c_stack(self):
        work = ROOT / 'build' / 'tests' / 'continuations'
        work.mkdir(parents=True, exist_ok=True)
        # A callee restores the landing-pad address into LR, as libunwind does.
        words = {0x1000: 0xa9bf7bfd, 0x1004: 0x940000bf, 0x1008: 0x00000000,
                 0x100c: 0xd28000e0, 0x1010: 0xa8c17bfd, 0x1014: 0xd65f03c0,
                 0x1100: 0xd282019e, 0x1104: 0xd65f03c0,
                 0x1200: 0x910043ff, 0x1204: 0xd282019e, 0x1208: 0xd65f03c0}
        import unicorn as uc
        from unicorn import arm64_const as a
        mu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
        mu.mem_map(0x1000, 0x3000)
        for address, word in words.items():
            mu.mem_write(address, struct.pack('<I', word))
        mu.mem_write(0x1300, struct.pack('<I', 0x17ffff80))  # branch to 0x1100
        mu.reg_write(a.UC_ARM64_REG_SP, 0x4000)
        mu.reg_write(a.UC_ARM64_REG_X30, 0x3000)
        mu.emu_start(0x1000, 0x3000, count=100)
        self.assertEqual(mu.reg_read(a.UC_ARM64_REG_X0), 7)
        self.assertEqual(mu.reg_read(a.UC_ARM64_REG_SP), 0x4000)
        lifter = Lifter(0, 'c')
        mod = SimpleNamespace(hi=0x120c, lift=lambda a: lifter.lift(words[a], a))
        with patch.object(eh, 'landing_pads', return_value={0x100c}):
            emitter = gen.Emitter(mod, {0x1000, 0x1100, 0x1200}, continuations=True)
        code = gen.chunk_text('synthetic', 0, emitter, [(0x1000, 0x1018), (0x1100, 0x1108), (0x1200, 0x120c)])
        (work / 'lifted.c').write_text(code)
        (work / 'test.c').write_text(r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "host.h"
void f_1000(Cpu *c); void f_1100(Cpu *c); void f_1200(Cpu *c);
static int nested;
const int rt_game_id=1;
volatile int g_fnprof;
void fnprof_hit(uint64_t a,uint64_t b,uint64_t d) { abort(); }
void rt_vlogf(const char *fmt,va_list ap) { vfprintf(stderr,fmt,ap); }
void rt_log_raw(const char *p,size_t n) { fwrite(p,1,n,stderr); }
void *plat_mem_alloc(size_t n) { return calloc(1,n); }
int plat_mem_protect(uint64_t a,uint64_t n,int p) { return 1; }
void plat_commit_info(double *a,double *b,double *c) { *a=*b=*c=0; }
void kern_backtrace(const Cpu *c) {}
void kern_checkpoint(void) {}
void rt_udf(Cpu *c,uint64_t pc) { fprintf(stderr,"unexpected instruction %#llx",(unsigned long long)pc); abort(); }
int kern_host_track; uint32_t rt_trace;
int ldr_count(void) { return 0; }
const char *ldr_describe(uint64_t a,char *b,int n) { return "synthetic"; }
void rt_log(const char *fmt,...) {}
int kern_core_away(void) { return 0; }
void kern_core_back(int *away) {}
void kern_host_left(void *thread) {}
int rt_linked_modules(RtModule *out,int n) {
 static const uint64_t addrs[]={0x1000,0x1100,0x1200};
 static const GuestFn fns[]={f_1000,f_1100,f_1200};
 *out=(RtModule){"synthetic",0x1000,0x20c,addrs,fns,3}; return 1;
}
static void callback(Cpu *c) {
 if(nested) { c->sp-=16; *kern_host_depth(c->thread)+=2; rt_call(c,0x1200,0); abort(); }
 f_1100(c); abort();
}
int main(void) {
 uint64_t stack[16]={0}; int thread[2]={0,3};
 Cpu c={.sp=(uint64_t)(stack+16),.thread=thread};
 rt_register_host(0x1300,callback);
 c.x[30]=0x1234; c.pc=0x5678;
 assert(rt_call(&c,0x1000,0)==7);
 assert(c.sp==(uint64_t)(stack+16)); assert(c.x[30]==0x1234 && c.pc==0x5678);
 assert(!c.call_frame && !c.resume_pc && thread[1]==3);
 nested=1;
 assert(rt_call(&c,0x1000,0)==7);
 assert(c.sp==(uint64_t)(stack+16) && !c.call_frame && thread[1]==3);
 puts("nonlocal landing-pad transfer passed");
}
''')
        cc = os.environ.get('CC', str(ROOT / 'toolchain/engage/llvm/bin/clang.exe'))
        exe = work / 'test.exe'
        command = [cc, '-O1', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        '-I', str(ROOT / 'runtime'), str(work / 'test.c'), str(work / 'lifted.c'),
                        str(ROOT / 'runtime/hostfn.c'), str(ROOT / 'runtime/dispatch.c'), '-o', str(exe)]
        compilation = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(compilation.returncode, 0, compilation.stderr)
        result = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
        self.assertIn('transfer passed', result.stdout)
        with patch.object(emitter, 'scan', return_value=(set(), {0x1004: 'all'})):
            fallback = gen.chunk_text('synthetic-fallback', 0, emitter,
                                      [(0x1000, 0x1018), (0x1100, 0x1108), (0x1200, 0x120c)])
        (work / 'lifted.c').write_text(fallback)
        compilation = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(compilation.returncode, 0, compilation.stderr)
        result = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
        self.assertIn('transfer passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
