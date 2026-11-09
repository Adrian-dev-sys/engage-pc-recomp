"""IL2CPP metadata-usage slots of main: the data words generated code loads a MethodInfo / class from, each holding an
encoded token until il2cpp initialises it (main+0x492060). Prints `<slot offset>\t<kind>\t<what>` for the method
references, so a hook can call a generic method through the same slot the game uses (initialise the slot, call the
MethodInfo's methodPointer with the MethodInfo as the last argument).

usage: il2cpp_usages.py --find "StructData<JobData>::Get"
"""
import argparse
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'recomp'))
sys.path.insert(0, str(ROOT / 'tools'))
from nso import Nso  # noqa: E402

TYPE_SIZE, METHOD_SIZE = 88, 32


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exefs', default=str(ROOT / 'build/extracted/engage-v200/ExeFS'))
    ap.add_argument('--metadata', default=str(ROOT / 'build/extracted/engage-v200/RomFS/Data/Managed/Metadata/'
                                                     'global-metadata.dat'))
    ap.add_argument('--find', default='')
    a = ap.parse_args()
    m = Path(a.metadata).read_bytes()
    f = struct.unpack_from('<64i', m, 8)
    sec = lambda i: (f[2 * i], f[2 * i + 1])
    so = sec(2)[0]
    s = lambda i: m[so + i:m.index(b'\0', so + i)].decode('utf-8', 'replace')
    to, tl = sec(19)
    tn = tl // TYPE_SIZE
    mo = sec(5)[0]
    nso = Nso(Path(a.exefs) / 'main')
    img = bytes(nso.image)
    rel = {x: add for x, _, _, add in nso.relocs}
    reg = 0
    for x in range(nso.data[0], nso.data[0] + nso.data[1] - 32, 8):
        if (struct.unpack_from('<Q', img, x)[0] == tn and struct.unpack_from('<Q', img, x + 16)[0] == tn
                and x + 8 in rel and x + 24 in rel):
            reg = x - 0x50   # Il2CppMetadataRegistration
            break
    q = lambda k: struct.unpack_from('<Q', img, reg + 8 * k)[0]
    n_insts, insts = q(2), rel.get(reg + 24, 0)
    n_types, types = q(6), rel.get(reg + 56, 0)
    n_specs, specs = q(8), rel.get(reg + 72, 0)

    def tdname(t):
        return s(struct.unpack_from('<i', m, to + t * TYPE_SIZE)[0])

    def type_name(p):
        bits = struct.unpack_from('<I', img, p + 8)[0]
        kind = bits >> 16 & 0xFF
        data = rel.get(p, struct.unpack_from('<Q', img, p)[0])
        if kind in (0x11, 0x12):
            return tdname(data & 0xFFFFFFFF)
        if kind == 0x1d:
            return type_name(data) + '[]'
        if kind == 0x15:
            return tdname(struct.unpack_from('<i', img, data)[0]) + '<>'
        return {0xe: 'string', 0x8: 'int', 0x2: 'bool', 0x1c: 'object'}.get(kind, f'#{kind:x}')

    def inst_name(i):
        if not 0 <= i < n_insts:
            return ''
        p = rel.get(insts + 8 * i, 0)
        argc = struct.unpack_from('<Q', img, p)[0]
        argv = rel.get(p + 8, 0)
        return ','.join(type_name(rel.get(argv + 8 * k, 0)) for k in range(argc))

    def method_name(mi):
        b = mo + mi * METHOD_SIZE
        name, decl = struct.unpack_from('<2i', m, b)
        np = struct.unpack_from('<H', m, b + 30)[0]
        ps = struct.unpack_from('<i', m, b + 12)[0]
        po = sec(10)[0]
        tys = []
        for k in range(ps, ps + np):
            ti = struct.unpack_from('<i', m, po + 12 * k + 8)[0]
            tys.append(type_name(rel.get(types + 8 * ti, 0)) if 0 <= ti < n_types else '?')
        return tdname(decl), s(name), ','.join(tys)

    for x in range(nso.data[0], nso.data[0] + nso.data[1], 8):
        v = struct.unpack_from('<Q', img, x)[0]
        if not v & 1 or v >> 32 or x in rel:
            continue
        kind, idx = v >> 29 & 7, (v & 0x1FFFFFFE) >> 1
        if kind == 1 and idx < n_types:   # 1: a TypeInfo (an Il2CppClass*, from an Il2CppType)
            text = type_name(rel.get(types + 8 * idx, 0))
            if a.find and a.find in 'class ' + text:
                print(f'{x:x}	class	{text}')
            continue
        if kind != 6 or idx >= n_specs:   # 6: a MethodRef (an Il2CppMethodSpec)
            continue
        md, ci, mi = struct.unpack_from('<3i', img, specs + 12 * idx)
        if not 0 <= md < sec(5)[1] // METHOD_SIZE:
            continue
        cls, name, np = method_name(md)
        text = f'{cls}<{inst_name(ci)}>::{name}' + (f'<{inst_name(mi)}>' if mi >= 0 else '') + f'({np})'
        if a.find in text:
            print(f'{x:x}\tmethod\t{text}')


if __name__ == '__main__':
    main()
