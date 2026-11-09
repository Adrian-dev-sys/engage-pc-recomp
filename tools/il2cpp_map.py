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
    return {'string': sec(2), 'methods': sec(5), 'params': sec(10), 'nested': sec(15), 'types': sec(19),
            'images': sec(20)}


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
    # Il2CppMetadataRegistration: found by its two type-definition counts (fieldOffsets, typeDefinitionsSizes).
    reg_types, n_types = 0, 0
    for a in range(nso.data[0], nso.data[0] + nso.data[1] - 32, 8):
        if (struct.unpack_from('<Q', img, a)[0] == tn and struct.unpack_from('<Q', img, a + 16)[0] == tn
                and a + 8 in rel and a + 24 in rel):
            n_types, reg_types = struct.unpack_from('<Q', img, a - 0x20)[0], rel.get(a - 0x18, 0)
            break
    prim = {1: 'void', 2: 'bool', 3: 'char', 4: 'sbyte', 5: 'byte', 6: 'short', 7: 'ushort', 8: 'int', 9: 'uint',
            0xa: 'long', 0xb: 'ulong', 0xc: 'float', 0xd: 'double', 0xe: 'string', 0x18: 'IntPtr', 0x1c: 'object'}

    def tdname(t):
        return s(struct.unpack_from('<i', m, to + t * TYPE_SIZE)[0])

    def type_name(p):   # p: address of an Il2CppType
        if not p:
            return '?'
        bits = struct.unpack_from('<I', img, p + 8)[0]
        kind, byref = bits >> 16 & 0xFF, bits >> 30 & 1
        data = rel.get(p, struct.unpack_from('<Q', img, p)[0])
        if kind in prim:
            name = prim[kind]
        elif kind in (0x11, 0x12):   # VALUETYPE, CLASS: a type definition index
            name = tdname(data & 0xFFFFFFFF)
        elif kind == 0x1d:           # SZARRAY: the element type
            name = type_name(data) + '[]'
        elif kind == 0x15:           # GENERICINST: Il2CppGenericClass, its type definition first
            name = tdname(struct.unpack_from('<i', img, data)[0]) + '<>'
        elif kind in (0x13, 0x1e):
            name = 'T'
        else:
            name = f'#{kind:x}'
        return name + ('&' if byref else '')

    def param_types(start, count):
        po = h['params'][0]
        out = []
        for q in range(start, start + count):
            ti = struct.unpack_from('<i', m, po + 12 * q + 8)[0]
            out.append(type_name(rel.get(reg_types + 8 * ti, 0)) if reg_types and 0 <= ti < n_types else '?')
        return ','.join(out)

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
                pstart = struct.unpack_from('<i', m, mb + 12)[0]
                static = ' static' if struct.unpack_from('<H', m, mb + 24)[0] & 0x10 else ''
                rid = token & 0xFFFFFF
                if not 0 < rid <= count:
                    continue
                ptr = rel.get(ptrs + (rid - 1) * 8, 0)
                if ptr:
                    out.append((f'{full}::{mname}({nparams}) [{param_types(pstart, nparams)}]{static}', ptr))
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
