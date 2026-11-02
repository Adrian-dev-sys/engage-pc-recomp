"""Audit an exact Git commit and package a source-only Engage prerelease."""
import argparse
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import subprocess
import tarfile
import zipfile

from check_content import BLOCKED_NAMES, BLOCKED_SUFFIXES, GENERATED_MARKER, KEY_LINE, MAX_BYTES

ROOT = Path(__file__).resolve().parents[1]
PRIVATE_DIRS = {'build', 'generated', 'toolchain', 'refs', '.git', '.venv', '.venv-py314', '.claude', '.agents', 'notes', '.worktrees'}
PRIVATE_NAMES = {'config.local.toml', '.env', 'account.uid', 'engage.ini', 'engage-install.json'}
BINARY_SUFFIXES = {'.exe', '.dll', '.pdb', '.o', '.obj', '.a', '.lib', '.log', '.png', '.jpg', '.jpeg', '.ico'}


def audit_member(name, data, regular=True):
    path = PurePosixPath(name)
    if not regular or path.is_absolute() or '..' in path.parts or '\\' in name:
        raise ValueError(f'Unsafe archive entry: {name}')
    if any(part in PRIVATE_DIRS for part in path.parts) or path.name in PRIVATE_NAMES:
        raise ValueError(f'Private build/configuration path: {name}')
    suffix = path.suffix.lower()
    if path.name.lower() in BLOCKED_NAMES or suffix in BLOCKED_SUFFIXES:
        raise ValueError(f'Game content path: {name}')
    # This unchanged upstream logo is licensed source artwork, not an extracted game icon.
    if suffix in BINARY_SUFFIXES and name != 'runtime/totk.ico':
        raise ValueError(f'Binary, log or unreviewed artwork: {name}')
    if len(data) > MAX_BYTES:
        raise ValueError(f'Source entry exceeds content-guard limit: {name}')
    if GENERATED_MARKER.encode() in data or data.startswith((b'NSO0', b'NCA2', b'NCA3', b'PFS0', b'MZ')):
        raise ValueError(f'Generated code or executable/container content: {name}')
    if KEY_LINE.search(data.decode('utf-8', errors='ignore')):
        raise ValueError(f'Key material in source entry: {name}')


def make_release(ref, out):
    commit = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', '--verify', ref + '^{commit}'], text=True).strip()
    raw = subprocess.check_output(['git', '-C', str(ROOT), 'archive', '--format=tar', commit])
    files = []
    with tarfile.open(fileobj=io.BytesIO(raw), mode='r:') as archive:
        for member in archive:
            if member.isdir():
                continue
            stream = archive.extractfile(member) if member.isfile() else None
            data = stream.read() if stream else b''
            audit_member(member.name, data, member.isfile())
            files.append((member.name, data, member.mode))
    version = dict((name, data) for name, data, _ in files)['VERSION'].decode().strip()
    if not version or any(c not in '0123456789abcdefghijklmnopqrstuvwxyz.-' for c in version):
        raise ValueError('VERSION is not a safe source-prerelease version')
    if ref.startswith('engage-v') and ref != 'engage-v' + version:
        raise ValueError('Tag and VERSION disagree')
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    filename = f'Engage-PC-Recomp-{version}-source.zip'
    destination = out / filename
    partial = out / (filename + '.partial')
    if destination.exists() or partial.exists():
        raise ValueError('Release output exists; use a new output directory')
    prefix = filename[:-4]
    with zipfile.ZipFile(partial, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for name, data, mode in sorted(files):
            info = zipfile.ZipInfo(prefix + '/' + name, (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o100000 | mode) << 16
            info.create_system = 3
            archive.writestr(info, data)
    partial.rename(destination)
    with destination.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    (out / 'SHA256SUMS').write_text(f'{digest}  {filename}\n', encoding='ascii')
    inventory = {'kind': 'source-only-preview', 'commit': commit, 'version': version,
                 'archive': filename, 'sha256': digest,
                 'files': [{'path': n, 'size': len(d), 'sha256': hashlib.sha256(d).hexdigest()} for n, d, _ in sorted(files)]}
    (out / 'source-inventory.json').write_text(json.dumps(inventory, indent=2) + '\n', encoding='utf-8')
    print(f'Audited {len(files)} source files at {commit}; wrote {destination.name}')
    return destination


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ref', default='HEAD')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    try:
        make_release(args.ref, args.out)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'{error}\n')
