"""Bundle an Engage setup candidate, never a compiled game or user data.

Run with Python 3.14. Downloads use the upstream release builder's pinned hashes.
The shader translator must have been built locally from the attributed source.
"""
import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import tarfile
import tempfile
from pathlib import Path
import zipfile

import make_release as upstream
from extract_msi import extract

ROOT = Path(__file__).resolve().parents[1]
upstream.PINS['engage-ninja-license'] = (
    'https://raw.githubusercontent.com/ninja-build/ninja/v1.13.2/COPYING',
    'eb7e9ab9690124c5c9f42bdc81383d886a3dede26345b6ed15bbad7caf81f7ea')


def tcltk_files():
    dest = upstream.WORK / 'tcltk'
    if (dest / 'DLLs/_tkinter.pyd').is_file():
        return dest
    data = upstream.fetch('python-installer').read_bytes()
    cabinets = []
    pos = data.find(b'MSCF\0\0\0\0')
    while pos >= 0:
        cabinets.append((struct.unpack_from('<I', data, pos + 8)[0], pos))
        pos = data.find(b'MSCF\0\0\0\0', pos + 1)
    size, pos = max(cabinets)
    work = upstream.WORK / 'engage-tcltk'
    work.mkdir(parents=True, exist_ok=True)
    cab = work / 'container.cab'
    cab.write_bytes(data[pos:pos + size])
    name, expected = upstream.TCLTK_PAYLOAD
    upstream.run(['expand.exe', '-F:' + name, cab, work], stdout=subprocess.DEVNULL)
    payload = work / name
    if upstream.sha256_file(payload) != expected:
        raise ValueError('Python Tcl/Tk component hash differs from the pinned release')
    extract(payload, dest)
    return dest


def setup_icon(path):
    """An original E monogram in a blue tile; contains no game artwork."""
    size = 32
    pixels = bytearray()
    for y in reversed(range(size)):
        for x in range(size):
            letter = 8 <= x < 12 and 7 <= y < 25 or 8 <= x < 24 and (7 <= y < 11 or 14 <= y < 18 or 21 <= y < 25)
            pixels.extend((255, 255, 255, 255) if letter else (145, 62, 25, 255))
    dib = struct.pack('<IIIHHIIIIII', 40, size, size * 2, 1, 32, 0, len(pixels), 0, 0, 0, 0) + pixels + bytes(128)
    path.write_bytes(struct.pack('<HHH', 0, 1, 1) + struct.pack('<BBBBHHII', size, size, 0, 0, 1, 32, len(dib), 22) + dib)


def copy_sources(out):
    for directory in ('recomp', 'installer', 'engage_setup', 'tools'):
        target = out / directory
        if not target.resolve().is_relative_to(out.resolve()):
            raise ValueError('Setup source folder is a link outside the bundle')
        if target.exists():
            shutil.rmtree(target)
    public = subprocess.check_output(['git', '-C', str(ROOT), 'ls-files', '-z', '--cached', '--others', '--exclude-standard'])
    for name in public.decode().split('\0'):
        relative = Path(name)
        if not name or relative.parts[0] not in ('runtime', 'recomp', 'installer', 'engage_setup', 'tools'):
            continue
        source = ROOT / relative
        if source.suffix.lower() not in {'.py', '.c', '.cc', '.cpp', '.h', '.hpp', '.json', '.ps1', '.txt',
                                        '.manifest', '.inl', '.glsl', '.comp', '.spv', '.inc'}:
            continue
        target = out / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    for name in ('VERSION', 'LICENSE', 'THIRD_PARTY_NOTICES.md'):
        shutil.copy2(ROOT / name, out / name)


def licenses(out):
    lic = out / 'licenses'
    lic.mkdir(exist_ok=True)
    pairs = [('opus', 'opus/COPYING'), ('vulkan-headers', 'vulkan-headers/LICENSE'),
             ('vulkan-loader', 'vulkan-loader/LICENSE'), ('vma', 'VulkanMemoryAllocator/LICENSE'),
             ('zstd', 'zstd/LICENSE')]
    for package, relative in pairs:
        shutil.copy2(upstream.unpack(package) / 'ucrt64/share/licenses' / relative, lic / (package + '-LICENSE.txt'))
    shutil.copy2(upstream.unpack('sdl3-dll') / 'LICENSE.txt', lic / 'SDL3-LICENSE.txt')
    shutil.copy2(out / 'llvm/LICENSE.TXT', lic / 'llvm-mingw-LICENSE.txt')
    shutil.copy2(ROOT / 'runtime/shaders/ffx/LICENSE.txt', lic / 'FidelityFX-FSR-LICENSE.txt')
    shutil.copy2(ROOT / 'tools/nx_extract/LICENSE', lic / 'nxnative-extractor-LICENSE.txt')
    shutil.copy2(ROOT / 'tools/nx_extract/LibHac-LICENSE.txt', lic / 'LibHac-LICENSE.txt')
    shutil.copy2(ROOT / 'tools/vendor/macnativegames/LICENSE', lic / 'macnativegames-LICENSE.txt')
    shutil.copy2(ROOT / 'tools/vendor/ryujinx/LICENSE.txt', lic / 'ryujinx-LICENSE.txt')
    dotnet = Path.home() / '.nuget/packages/microsoft.netcore.app.runtime.win-x64/10.0.13'
    for name in ('LICENSE.TXT', 'THIRD-PARTY-NOTICES.TXT'):
        shutil.copy2(dotnet / name, lic / ('dotnet-' + name))
    ninja = upstream.fetch('engage-ninja-license')
    shutil.copy2(ninja, lic / 'ninja-LICENSE.txt')
    with tarfile.open(upstream.fetch('libvpx-src')) as archive:
        for name in ('LICENSE', 'PATENTS'):
            (lic / ('libvpx-' + name + '.txt')).write_bytes(archive.extractfile('libvpx-1.17.0/' + name).read())


def build(out):
    out = out.resolve()
    if not out.is_relative_to((ROOT / 'build/release').resolve()):
        raise ValueError('Setup candidates must be built inside build/release')
    out.mkdir(parents=True, exist_ok=True)
    upstream.tcltk_files = tcltk_files
    upstream.build_python(out)
    upstream.build_llvm(out)
    upstream.build_deps(out)
    copy_sources(out)
    binary = out / 'bin'
    binary.mkdir(exist_ok=True)
    for name in ('totk_xlate.exe', 'totk_xlate.id'):
        shutil.copy2(ROOT / 'build/xlate' / name, binary / name)
    depsbin = out / 'runtime/deps/bin'
    depsbin.mkdir(exist_ok=True)
    sdl = upstream.unpack('sdl3-dll') / 'SDL3.dll'
    if upstream.sha256_file(sdl) != upstream.SDL3_DLL_SHA256:
        raise ValueError('SDL3 DLL hash differs from pinned download')
    shutil.copy2(sdl, depsbin / sdl.name)
    shutil.copy2(upstream.unpack('opus') / 'ucrt64/bin/libopus-0.dll', depsbin / 'libopus-0.dll')
    shutil.copy2(upstream.unpack('ninja') / 'ninja.exe', binary / 'ninja.exe')
    upstream.run(['dotnet', 'publish', ROOT / 'tools/nx_extract/nx_extract.csproj', '-c', 'Release',
        '-r', 'win-x64', '--self-contained', 'true', '--artifacts-path', ROOT / 'build/nx-extract-release-artifacts',
        '-p:RuntimeFrameworkVersion=10.0.13', '-o', binary / 'nx_extract'])
    for symbols in (binary / 'nx_extract').glob('*.pdb'):
        symbols.unlink()
    clang = out / 'llvm/bin/clang.exe'
    upstream.run([clang, '-O2', '-I' + str(out / 'runtime/deps/include'),
                  ROOT / 'installer/hostcheck.c', '-o', binary / 'hostcheck.exe'])
    setup_icon(out / 'runtime/engage-setup.ico')
    launcher = (ROOT / 'installer/launcher/launcher.c').read_text().replace('-m installer.gui', '-m engage_setup.gui')
    work = ROOT / 'build/release/engage-launcher'
    work.mkdir(exist_ok=True)
    (work / 'launcher.c').write_text(launcher)
    (work / 'launcher_text.h').write_text('''#define LAUNCHER_TITLE_EN L"Engage Setup"
#define LAUNCHER_TITLE_RU LAUNCHER_TITLE_EN
#define LAUNCHER_PYTHON_MISSING_EN L"Extract the entire Engage-Setup ZIP, then run Engage-Setup.exe again."
#define LAUNCHER_PYTHON_MISSING_RU LAUNCHER_PYTHON_MISSING_EN
''')
    (work / 'launcher.rc').write_text(f'1 ICON "{(out / "runtime/engage-setup.ico").as_posix()}"\n1 24 "{(ROOT / "runtime/totk.manifest").as_posix()}"\n')
    upstream.run([out / 'llvm/bin/llvm-windres.exe', work / 'launcher.rc', '-O', 'coff', '-o', work / 'launcher.o'])
    upstream.run([clang, '-O2', '-mwindows', work / 'launcher.c', work / 'launcher.o', '-o', out / 'Engage-Setup.exe'])
    licenses(out)
    (out / 'START-HERE.txt').write_text('Engage PC Recomp — experimental setup\n\n'
        '1. Extract this entire ZIP into a folder.\n2. Run Engage-Setup.exe.\n'
        '3. Choose your own base game, update 2.0.0, prod.keys and extracted firmware folder.\n'
        '4. Optionally add either DLC package and title.keys, then choose a separate install folder.\n'
        '5. Select Install. When it finishes, select Play Engage.\n\n'
        'Requirements: Windows 10/11 x64, AVX2 CPU, Vulkan 1.3 GPU/driver, 40 GiB free.\n'
        'Setup builds the game locally. No game files or keys are included.\n'
        'First install may take 20–60 minutes. Shaders cache during play.\n'
        'Keep your original dumps. Saves/settings are in your chosen install folder.\n'
        'Help: https://github.com/veneito/engage-pc-recomp\n', encoding='utf-8')
    upstream.run([out / 'python/python.exe', '-I', '-m', 'engage_setup', '--help'], env=upstream.clean_env())
    with tempfile.TemporaryDirectory(prefix='engage-no-input-') as temp:
        dest = Path(temp) / 'install'
        result = subprocess.run([out / 'python/python.exe', '-I', '-m', 'engage_setup', '--dest', dest],
                                env=upstream.clean_env(), capture_output=True, text=True, timeout=30)
        events = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
        if result.returncode != 1 or not events or events[-1].get('result') != 'failed' or dest.exists():
            raise ValueError('The bundled missing-input failure check did not pass')
    upstream.run([out / 'python/python.exe', '-I', '-m', 'engage_setup.gui', '--selftest'], env=upstream.clean_env())
    for cache in sorted(out.rglob('__pycache__'), key=lambda p: len(p.parts), reverse=True):
        if not cache.resolve().is_relative_to(out):
            raise ValueError('Unexpected cache link outside setup')
        shutil.rmtree(cache)
    forbidden = {'engage.exe', 'engage-install.json', 'config.local.toml', 'account.uid', 'engage.ini'}
    for file in out.rglob('*'):
        compiler_object = file.suffix.lower() == '.o' and file.is_relative_to(out / 'llvm')
        if file.name.lower() in forbidden or file.suffix.lower() in ('.log', '.pdb', '.obj', '.o') and not compiler_object:
            raise ValueError(f'Private build artifact in setup: {file.relative_to(out)}')
    upstream.check_folder(out)
    inventory = []
    for file in sorted(out.rglob('*')):
        if file.is_file() and '__pycache__' not in file.parts and file.name != 'setup-inventory.json':
            inventory.append({'path': file.relative_to(out).as_posix(), 'size': file.stat().st_size,
                              'sha256': upstream.sha256_file(file)})
    (out / 'setup-inventory.json').write_text(json.dumps(inventory, indent=2) + '\n')
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--zip', type=Path)
    args = parser.parse_args()
    if args.zip and subprocess.check_output(['git', '-C', str(ROOT), 'status', '--porcelain']).strip():
        parser.error('Commit the reviewed source before making a public ZIP. Omit --zip for a development candidate.')
    out = build(args.out)
    if args.zip:
        with zipfile.ZipFile(args.zip, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
            for file in sorted(out.rglob('*')):
                if file.is_file() and '__pycache__' not in file.parts:
                    archive.write(file, Path('Engage-Setup-' + (out / 'VERSION').read_text().strip() + '-windows') / file.relative_to(out))
        checksum = upstream.sha256_file(args.zip)
        (args.zip.parent / 'SHA256SUMS').write_text(f'{checksum}  {args.zip.name}\n', encoding='utf-8')
        print('SHA256', checksum, args.zip.name)


if __name__ == '__main__':
    main()
