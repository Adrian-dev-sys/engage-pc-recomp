"""Verify an extracted Engage 2.0.0 ExeFS before running the upstream lifter."""
import argparse
import hashlib
import json
from pathlib import Path

from check_content import GENERATED_MARKER

ROOT = Path(__file__).resolve().parents[1]
# Build-ID prefixes recorded in nxnative game.tsv at 97f1514a46858b5f48c648022c081b536fbc6ad5.
BUILDS = {
    'main': '8c08b9719e085f91847b5e0f935d9488',
    'subsdk0': 'bffed55ea46c1b48ddeabcacdfbf81bf',
    'sdk': '56ec24747ec9e7f19b3efb062483a19d',
}
DLC_SPECS = {'0100a6301214f001': (1, 'e0'), '0100a6301214f002': (2, 'h0')}


def inspect_dlcs(root, require_all=True):
    report = []
    for title, (index, marker) in DLC_SPECS.items():
        path = root / str(index) / marker
        if not require_all and not (root / str(index)).exists():
            continue
        if not path.is_file() or path.stat().st_size != 0:
            raise ValueError(f'DLC {index} must contain its empty {marker} marker from the package')
        report.append({'title_id': title.upper(), 'index': index, 'marker': marker})
    return report


def inspect_module(path, expected):
    with path.open('rb') as stream:
        header = stream.read(0x100)
        if len(header) != 0x100 or header[:4] != b'NSO0':
            raise ValueError(f'{path.name}: expected an NSO0 module with a complete header')
        build_id = header[0x40:0x60].hex()
        if not build_id.startswith(expected):
            raise ValueError(f'{path.name}: build ID does not match Engage 2.0.0; no patches applied')
        stream.seek(0)
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    return {'build_id': build_id, 'sha256': digest, 'size': path.stat().st_size}


def inspect_inputs(exefs, romfs=None):
    modules = {name: inspect_module(exefs / name, build) for name, build in BUILDS.items()}
    if romfs is not None and not (romfs / 'Data' / 'data.unity3d').is_file():
        raise ValueError('RomFS must be an extracted tree containing Data/data.unity3d')
    return {
        'marker': GENERATED_MARKER,
        'game': 'Fire Emblem Engage', 'title_id': '0100A6301214E000', 'version': '2.0.0',
        'modules': modules,
        'exefs_sha256': {name: module['sha256'] for name, module in modules.items()},
        'romfs_marker_present': romfs is not None,
        'note': 'Local inventory only; not an independent integrity check or proof of runtime compatibility.',
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exefs', type=Path, required=True)
    parser.add_argument('--romfs', type=Path)
    parser.add_argument('--aoc', type=Path, help='extracted DLC root containing 1/e0 and 2/h0')
    args = parser.parse_args()
    try:
        report = inspect_inputs(args.exefs, args.romfs)
        if args.aoc:
            report['dlc'] = inspect_dlcs(args.aoc)
    except (OSError, ValueError) as error:
        parser.exit(1, f'{error}\n')
    out = ROOT / 'build' / 'engage-v200' / 'inputs.json'
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print('Engage 2.0.0 module build IDs match. Local inventory: build/engage-v200/inputs.json')
    print('Next: instruction coverage, then lift without any TotK patches. Runtime port is not ready.')


if __name__ == '__main__':
    main()
