"""Pack the expanded Engage RomFS for the runtime's indexed image reader."""
import argparse
import hashlib
import json
import os
import stat
from pathlib import Path

from check_content import GENERATED_MARKER


def pack(source, destination):
    source, destination = source.resolve(), destination.resolve()
    if destination == source or destination.is_relative_to(source):
        raise ValueError('Destination must be outside the source tree')
    if destination.exists():
        raise ValueError('Destination already exists; inspect it before replacing any image')
    files = []
    for folder, directories, names in os.walk(source, followlinks=False):
        for name in directories:
            path = Path(folder) / name
            if path.is_symlink() or path.is_junction():
                raise ValueError('Source contains a directory link')
        files.extend(Path(folder) / name for name in names)
    files.sort(key=lambda p: p.relative_to(source).as_posix().encode())
    if not files:
        raise ValueError('Source tree is empty')
    destination.mkdir(parents=True)
    digest, entries = hashlib.sha256(), []
    partial = destination / 'romfs.bin.part'
    with partial.open('xb') as output:
        for path in files:
            before = path.lstat()
            if not stat.S_ISREG(before.st_mode) or getattr(before, 'st_file_attributes', 0) & 0x400:
                raise ValueError('Source contains a file link or special file')
            offset = output.tell()
            with path.open('rb') as stream:
                for block in iter(lambda: stream.read(4 << 20), b''):
                    output.write(block)
                    digest.update(block)
            after = path.stat()
            if before.st_size != output.tell() - offset or before.st_mtime_ns != after.st_mtime_ns:
                raise ValueError('Source changed while packing')
            entries.append({'path': path.relative_to(source).as_posix(), 'offset': offset, 'size': before.st_size})
        size = output.tell()
    report = {'marker': GENERATED_MARKER, 'status': 'pass', 'romfs_size': size,
              'romfs_sha256': digest.hexdigest(), 'files': entries,
              'integrity': 'Packed from the local tree; extraction integrity is recorded separately by LibHac.'}
    partial.rename(destination / 'romfs.bin')
    (destination / 'index.json').write_text(json.dumps(report, indent=1) + '\n', encoding='utf-8')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    report = pack(args.source, args.out)
    print(f'Packed {len(report["files"])} files, {report["romfs_size"]} bytes')
