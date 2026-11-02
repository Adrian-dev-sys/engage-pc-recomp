"""Build the Engage 2.0.0 bring-up executable with the local pinned toolchain."""
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

import build_game
from engage_inputs import ROOT, inspect_dlcs, inspect_inputs


def main():
    extracted = ROOT / 'build' / 'extracted' / 'engage-v200'
    report = inspect_inputs(extracted / 'ExeFS', extracted / 'RomFS')
    report['dlc'] = inspect_dlcs(extracted / 'aoc')
    npdm = extracted / 'ExeFS' / 'main.npdm'
    report['exefs_sha256']['main.npdm'] = hashlib.sha256(npdm.read_bytes()).hexdigest()
    expected = ROOT / 'build' / 'engage-v200' / 'expected.json'
    expected.parent.mkdir(parents=True, exist_ok=True)
    expected.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    output = Path(sys.argv[sys.argv.index('--out') + 1]).resolve() if '--out' in sys.argv else ROOT / 'build' / 'engage-v200'
    output.mkdir(parents=True, exist_ok=True)
    icon = output / 'engage.ico'
    source = extracted / 'icon.jpg'
    icon_tool = ROOT / 'tools' / 'make_engage_icon.ps1'
    if not icon.exists() or icon.stat().st_mtime < max(source.stat().st_mtime, icon_tool.stat().st_mtime):
        subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(icon_tool),
                        '-Source', str(source), '-Output', str(icon)], check=True)
    resource = output / 'engage.rc'
    content = f'''1 24 "{(ROOT / 'runtime' / 'totk.manifest').as_posix()}"
1 ICON "{icon.as_posix()}"
1 VERSIONINFO
FILEVERSION 0,1,0,0
PRODUCTVERSION 0,1,0,0
FILEOS 0x40004
FILETYPE 1
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904b0"
    BEGIN
      VALUE "FileDescription", "Engage PC Recomp\\0"
      VALUE "ProductName", "Engage PC Recomp\\0"
      VALUE "FileVersion", "0.1.0\\0"
      VALUE "OriginalFilename", "engage.exe\\0"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
'''
    build_game.write_if_changed(resource, content)
    original_sources = build_game.runtime_sources
    build_game.runtime_sources = lambda skip=(): [resource if p.name == 'totk.rc' else p for p in original_sources(skip)]
    build_game.APP_ICON = icon
    old_ini, new_ini = output / 'totk.ini', output / 'engage.ini'
    if old_ini.exists() and not new_ini.exists():
        shutil.copy2(old_ini, new_ini)
    build_game.EXE = 'engage.exe'
    sys.argv = ['build_game.py', '--version', 'engage-v200', '--expected', str(expected),
                '--exefs', str(extracted / 'ExeFS'), '--romfs', str(extracted / 'packed-romfs'),
                '--profile', str(ROOT / 'build' / 'profile-engage'),
                '--toolchain', str(ROOT / 'toolchain' / 'engage' / 'llvm'),
                '--deps', str(ROOT / 'toolchain' / 'engage' / 'runtime' / 'deps'),
                '--ninja', str(ROOT / 'toolchain' / 'engage' / 'ninja.exe'), *sys.argv[1:]]
    build_game.main()
    output = Path(sys.argv[sys.argv.index('--out') + 1]) if '--out' in sys.argv else ROOT / 'build' / 'engage-v200'
    if '--compile-only' not in sys.argv:
        for name in ('totk_xlate.exe', 'totk_xlate.id'):
            source = ROOT / 'build' / 'xlate' / name
            if source.is_file():
                shutil.copy2(source, output / name)


if __name__ == '__main__':
    main()
