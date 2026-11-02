"""Engage-specific inputs; the upstream installer supplies the window and progress model."""
from dataclasses import dataclass
import json
from pathlib import Path
from types import SimpleNamespace

from installer.guilogic import Catalog as BaseCatalog, Form as BaseForm
from installer import ini

TITLE = '0100A6301214E000'
FIELDS = ('base', 'update', 'keys', 'firmware', 'dlc1', 'dlc2', 'title_keys')
TEXT = {
    'ui.title': 'Engage Setup',
    'ui.intro': 'Choose your game files and an install folder. Setup builds Engage on this PC.\nThe first installation can take 20–60 minutes; you can follow each step below.',
    'ui.notice': 'Experimental alpha. Opening scenes, audio and the first battle have been tested.\nFull-game completion and long-session stability remain unverified.',
    'ui.base': 'Base game (NSP / XCI)', 'ui.update': 'Update 2.0.0 (NSP)',
    'ui.firmware': 'Firmware NCA folder', 'ui.dlc1': 'Expansion Pass (optional)',
    'ui.dlc2': 'Order of Heroes (optional)', 'ui.title_keys': 'title.keys (optional)',
    'ui.keys_private': 'Keys are read from the selected files, never copied into the install folder.\nKeep your game dumps and keys private. Allow at least 40 GiB free on the install drive.',
    'ui.need_update': 'Choose the Engage 2.0.0 update.',
    'ui.need_firmware': 'Choose an extracted firmware NCA folder.',
    'ui.missing_firmware': 'The firmware folder does not exist.',
    'ui.done_text': 'Engage is installed in {dest}. Select Play Engage to start.\nKeep this setup package if you need to rebuild. Your original dumps are unchanged.',
    'ui.update_mode': 'This folder contains an Engage installation. Completed work can be resumed.\nUse the same input files when resuming an interrupted install.',
    'ui.play': 'Play Engage',
    'ui.start': 'Install Engage',
    'setup.failure': '{message}',
    'step.prepare': 'Check and finish',
    'step.compile': 'Compile Engage',
    'phase.extract.merge': 'Verifying and merging the base game and update…',
    'phase.extract.fonts': 'Extracting and verifying shared fonts…',
    'phase.extract.pack': 'Packing game data on the install drive…',
    'phase.lift.work': 'Recompiling ARM64 game code for this PC…',
    'phase.compile.work': 'Compiling the native executable…',
    'phase.prepare.work': 'Checking the build and installing local files…',
}
for name in ('dlc1', 'dlc2', 'title_keys'):
    TEXT['ui.missing_' + name] = f'The selected {name.replace("_", ".")} file does not exist.'


class Catalog(BaseCatalog):
    def set_language(self, lang):
        super().set_language('en')
        self.texts.update(TEXT)


def installed_state(dest):
    try:
        data = json.loads((Path(dest) / 'engage-install.json').read_text(encoding='utf-8'))
        return data if data.get('title_id') == TITLE and data.get('format') == 1 else None
    except (OSError, ValueError, TypeError, AttributeError):
        return None


@dataclass
class Form(BaseForm):
    firmware: str = ''
    dlc1: str = ''
    dlc2: str = ''
    title_keys: str = ''

    def installed_state(self):
        return installed_state(self.dest) if self.dest.strip() else None

    def problems(self):
        # Inputs remain required on resume: this prevents silently changing the DLC set.
        found = []
        if not self.dest.strip():
            found.append('ui.need_dest')
        for name in FIELDS:
            value = getattr(self, name).strip()
            if not value and name in FIELDS[:4]:
                found.append('ui.need_' + name)
            elif value and not (Path(value).is_dir() if name == 'firmware' else Path(value).is_file()):
                found.append('ui.missing_' + name)
        if self.lang not in ini.LANGUAGES:
            found.append('ui.need_lang')
        return found

    def command(self, python, root=None, temp=None, verify=False):
        command = [str(python), '-X', 'utf8', '-m', 'engage_setup']
        for name in (*FIELDS, 'lang', 'dest'):
            value = getattr(self, name).strip()
            if value:
                command += ['--' + name.replace('_', '-'), value]
        if root:
            command += ['--root', str(root)]
        return command


def layout(root=None):
    root = Path(root or Path(__file__).resolve().parents[1]).resolve()
    dev = not (root / 'python/python.exe').is_file()
    return SimpleNamespace(root=root, dev=dev, release=(root / 'VERSION').read_text().strip(),
        python=root / '.venv-py314/Scripts/python.exe' if dev else root / 'python/python.exe',
        runtime=root / 'runtime', llvm=root / 'toolchain/engage/llvm' if dev else root / 'llvm',
        deps=root / 'toolchain/engage/runtime/deps' if dev else root / 'runtime/deps',
        ninja=root / 'toolchain/engage/ninja.exe' if dev else root / 'bin/ninja.exe',
        extractor=root / 'toolchain/nx_extract/nx_extract.exe' if dev else root / 'bin/nx_extract/nx_extract.exe',
        xlate=root / 'build/xlate/totk_xlate.exe' if dev else root / 'bin/totk_xlate.exe',
        hostcheck=root / 'build/release/engage-hostcheck.exe' if dev else root / 'bin/hostcheck.exe')
