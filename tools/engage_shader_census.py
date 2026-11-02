"""Convert nxnative's extracted Unity shader packages into this runtime's probe jobs."""
import hashlib
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def records(data):
    offset = 0
    while offset < len(data):
        if data[offset:offset + 8] != b'BTCDBTCD' or len(data) - offset < 0x38:
            raise ValueError('Invalid shader package record header')
        control_size, code_size = struct.unpack_from('<II', data, offset + 0x28)
        start, end = offset + 0x38, offset + 0x38 + code_size + control_size
        if end > len(data) or control_size < 0x708:
            raise ValueError('Truncated shader package record')
        code = data[start:start + code_size]
        control = data[start + code_size:end]
        if control[:4] != bytes.fromhex('34127698'):
            raise ValueError('Invalid shader control section')
        yield control, code
        offset = end


def main():
    from shader_census import write_shaders
    work = ROOT / 'build' / 'engage-v200'
    shaders = {}
    packages = sorted((work / 'shader-package').glob('*.bin'))
    if not packages:
        raise ValueError('Run the pinned nxnative shader_split.py on Engage first')
    sources = {}
    for line in (work / 'shader-map.tsv').read_text(encoding='utf-8').splitlines():
        if not line or line.startswith('#') or line.startswith('package\t'):
            continue
        package, index, _, _, source = line.split('\t', 4)
        sources[package, int(index)] = source
    if not sources:
        raise ValueError('The shader source map is incomplete; rerun shader_split.py with PYTHONUTF8=1')
    skipped = 0
    for package in packages:
        stage = package.name.split('_', 1)[0]
        for index, (control, code) in enumerate(records(package.read_bytes())):
            if stage not in ('vs', 'ps', 'cs'):
                skipped += 1
                continue
            c1, end = struct.unpack_from('<II', control, 0x700)
            if not 0 <= c1 <= end <= len(code) or (stage != 'cs' and end < 0x30):
                raise ValueError('Shader code/control lengths disagree')
            key = hashlib.sha256(control[:0x880] + code).hexdigest()
            shaders[key] = {'control': control, 'code': code, 'stage': stage, 'startup': True,
                            'origin': 'unity', 'source': sources[package.name, index]}
    output = work / 'shaders'
    output.mkdir(exist_ok=True)
    jobs = write_shaders(shaders, output, True)
    print(f'Wrote {len(jobs)} Unity shader probe jobs; {skipped} other-stage records retained in packages')


if __name__ == '__main__':
    main()
