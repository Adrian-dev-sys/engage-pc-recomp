"""IL2CPP method map: every managed method of the game with its offset in main, for hook lines in
recomp/patches-engage-v200.txt.

Reads global-metadata.dat (version 27: Unity 2020.3) and main's Il2CppCodeGenModule table, which holds one method
pointer per method token of each assembly. Writes `<offset>\t<Namespace.Class>::<Method>(<parameter count>)` lines,
sorted by name, to build/<ver>/il2cpp-methods.tsv (game-derived: stays under build/).

usage: il2cpp_map.py [--exefs build/extracted/engage-v200/ExeFS] [--metadata .../global-metadata.dat] [--out ...]
       il2cpp_map.py --find App.GameSound::LoadDefaultSoundBanks   (prints matching lines)
"""
import argparse
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'recomp'))
from nso import Nso  # noqa: E402

TYPE_SIZE, METHOD_SIZE, IMAGE_SIZE = 88, 32, 40


def header(m):
    magic, ver = struct.unpack_from('<Ii', m, 0)
    if magic != 0xFAB11BAF or ver != 27:
        raise SystemExit(f'metadata magic {magic:#x} version {ver}: only version 27 is read')
    f = struct.unpack_from('<64i', m, 8)
    sec = lambda i: (f[2 * i], f[2 * i + 1])   # (offset, size) of header section i
    return {'string': sec(2), 'methods': sec(5), 'nested': sec(15), 'types': sec(19), 'images': sec(20)}


def cstr(b, o):
    return b[o:b.index(b'\0', o)].decode('utf-8', 'replace')


def build(exefs, metadata):
    m = Path(metadata).read_bytes()
    h = header(m)
    so = h['string'][0]
    s = lambda i: cstr(m, so + i)
    to, tn = h['types'][0], h['types'][1] // TYPE_SIZE
    mo = h['methods'][0]
    io, n_img = h['images'][0], h['images'][1] // IMAGE_SIZE
    outer = {}   # nested type definition -> its declaring one
    no = h['nested'][0]
    for t in range(tn):
        b = to + t * TYPE_SIZE
        start = struct.unpack_from('<i', m, b + 48)[0]
        count = struct.unpack_from('<H', m, b + 72)[0]
        for q in range(start, start + count):
            outer[struct.unpack_from('<i', m, no + 4 * q)[0]] = t
    nso = Nso(Path(exefs) / 'main')
    img = bytes(nso.image)
    rel = {a: add for a, _, _, add in nso.relocs}   # RELATIVE relocations: the pointers in .data
    by_target = {}
    for a, add in sorted(rel.items()):
        by_target.setdefault(add, a)
    out = []
    for k in range(n_img):
        name_i, _, type_start, type_count = struct.unpack_from('<4i', m, io + k * IMAGE_SIZE)
        name = s(name_i)
        at = img.find(name.encode() + b'\0')
        mod = by_target.get(at, -1)   # the Il2CppCodeGenModule: its moduleName points at the name
        if mod < 0:
            print(f'{name}: no code-gen module', file=sys.stderr)
            continue
        count = struct.unpack_from('<I', img, mod + 8)[0]
        ptrs = rel.get(mod + 16, 0)
        for t in range(type_start, type_start + type_count):
            b = to + t * TYPE_SIZE
            tname, tns = struct.unpack_from('<2i', m, b)
            method_start = struct.unpack_from('<i', m, b + 36)[0]
            method_count = struct.unpack_from('<H', m, b + 64)[0]
            cls, o = s(tname), t
            while o in outer:   # nested: Outer.Inner
                o = outer[o]
                d = to + o * TYPE_SIZE
                cls = s(struct.unpack_from('<i', m, d)[0]) + '.' + cls
                tns = struct.unpack_from('<i', m, d + 4)[0]
            full = (s(tns) + '.' if s(tns) else '') + cls
            for j in range(method_start, method_start + method_count):
                mb = mo + j * METHOD_SIZE
                mname = s(struct.unpack_from('<i', m, mb)[0])
                token = struct.unpack_from('<I', m, mb + 20)[0]
                nparams = struct.unpack_from('<H', m, mb + 30)[0]
                rid = token & 0xFFFFFF
                if not 0 < rid <= count:
                    continue
                ptr = rel.get(ptrs + (rid - 1) * 8, 0)
                if ptr:
                    out.append((f'{full}::{mname}({nparams})', ptr))
    out.sort()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exefs', default=str(ROOT / 'build/extracted/engage-v200/ExeFS'))
    ap.add_argument('--metadata', default=str(ROOT / 'build/extracted/engage-v200/RomFS/Data/Managed/Metadata/'
                                                     'global-metadata.dat'))
    ap.add_argument('--out', default=str(ROOT / 'build/engage-v200/il2cpp-methods.tsv'))
    ap.add_argument('--find', help='print the methods whose name contains this text')
    a = ap.parse_args()
    out = Path(a.out)
    if not out.exists():
        rows = build(a.exefs, a.metadata)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(''.join(f'{p:x}\t{n}\n' for n, p in rows), encoding='utf-8')
        print(f'{len(rows)} methods -> {out}', file=sys.stderr)
    if a.find:
        for line in out.read_text(encoding='utf-8').splitlines():
            if a.find in line:
                print(line)


if __name__ == '__main__':
    main()
