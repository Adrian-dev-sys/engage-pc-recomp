"""Extract Engage 2.0.0 and its two DLCs with the pinned nxnative extractor."""
import argparse
import json
import os
import re
import subprocess
import tomllib
from pathlib import Path

from engage_inputs import ROOT, DLC_SPECS, inspect_dlcs, inspect_inputs

TITLE = '0100a6301214e000'
UPDATE = '0100a6301214e800'
TOOL = ROOT / 'toolchain' / 'nx_extract' / 'nx_extract.exe'
OUT = ROOT / 'build' / 'extracted' / 'engage-v200'
LOGS = ROOT / 'build' / 'engage-v200' / 'logs'


def run_tool(keys, arguments, name, title_keys=None):
    result = subprocess.run([str(TOOL), str(keys), *map(str, arguments)],
                            capture_output=True, encoding='utf-8', errors='replace',
                            env=dict(os.environ, ENGAGE_TITLE_KEYS=str(title_keys) if title_keys else ''))
    # External tools must not persist or display possible key-sized values.
    output = re.sub(r'[0-9a-fA-F]{32,}', '<hex removed>', result.stdout + result.stderr)
    LOGS.mkdir(parents=True, exist_ok=True)
    (LOGS / f'{name}.log').write_text(output, encoding='utf-8')
    if result.returncode:
        raise ValueError(f'{name} failed (exit {result.returncode}); see {LOGS / (name + ".log")}')
    return output


def check_content(output, kind, title, version):
    entries = re.findall(r'^content (\w+) ([0-9a-fA-F]{16}) (\d+)\s*$', output, re.M)
    if [(k, t.lower(), int(v)) for k, t, v in entries] != [(kind, title.lower(), version)]:
        raise ValueError(f'Expected exactly {kind} {title} version {version}; package identity differs')


def main():
    global TOOL, OUT, LOGS
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, default=ROOT / 'config.local.toml')
    parser.add_argument('--out', type=Path, default=OUT)
    parser.add_argument('--tool', type=Path, default=TOOL)
    parser.add_argument('--logs', type=Path, default=LOGS)
    args = parser.parse_args()
    TOOL, OUT, LOGS = args.tool, args.out, args.logs
    try:
        data = tomllib.loads(args.config.read_text(encoding='utf-8'))['input']
        keys, base, update = (Path(data[name]) for name in ('keys', 'base', 'update'))
        title_keys = Path(data['title_keys']) if data.get('title_keys') else None
        dlcs = {title.lower(): Path(path) for title, path in data.get('dlc', {}).items()}
        if not set(dlcs).issubset(DLC_SPECS):
            raise ValueError('The config contains an unsupported DLC title ID')
        for path in (TOOL, keys, base, update, *dlcs.values()):
            if not path.is_file():
                raise ValueError(f'Missing input or extractor: {path}')
        if title_keys is not None and not title_keys.is_file():
            raise ValueError('Configured title_keys file is missing')
        for label, path, kind, title, version in (
            ('base', base, 'Application', TITLE, 0),
            ('update', update, 'Patch', UPDATE, 327680),
            *((f'dlc-{title}', path, 'AddOnContent', title, 0) for title, path in dlcs.items()),
        ):
            check_content(run_tool(keys, ['--contents', path], f'identity-{label}', title_keys), kind, title, version)
            print(f'Verified {label} package identity', flush=True)
        if (OUT / 'ExeFS').exists() or (OUT / 'RomFS').exists():
            raise ValueError('ExeFS/RomFS already exist: inspect the previous extraction before retrying')
        OUT.mkdir(parents=True, exist_ok=True)
        print('Extracting base + update with integrity checks; this can take several minutes', flush=True)
        output = run_tool(keys, [base, update, OUT, '--app', TITLE], 'base-update', title_keys)
        if not re.search(r'^display version 2\.0\.0\s*$', output, re.M):
            raise ValueError('Extracted control data did not report version 2.0.0')
        report = inspect_inputs(OUT / 'ExeFS', OUT / 'RomFS')
        for title, path in dlcs.items():
            index, _ = DLC_SPECS[title]
            destination = OUT / 'aoc' / str(index)
            run_tool(keys, ['--aoc', path, destination, title], f'dlc-{index}', title_keys)
        report['dlc'] = inspect_dlcs(OUT / 'aoc', require_all=False)
        (OUT / 'inputs.json').write_text(
            json.dumps(report, indent=2) + '\n', encoding='utf-8')
        print(f'Verified Engage 2.0.0 and extracted {len(dlcs)} DLC packages.')
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
