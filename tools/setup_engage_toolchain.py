"""Prepare the same hash-pinned compiler and runtime libraries as the TotK installer (Python 3.14)."""
import shutil
from pathlib import Path

import make_release


if __name__ == '__main__':
    output = Path(__file__).resolve().parents[1] / 'toolchain' / 'engage'
    make_release.build_llvm(output)
    make_release.build_deps(output)
    binaries = output / 'runtime' / 'deps' / 'bin'
    binaries.mkdir(parents=True, exist_ok=True)
    shutil.copy2(make_release.unpack('sdl3-dll') / 'SDL3.dll', binaries / 'SDL3.dll')
    shutil.copy2(make_release.unpack('opus') / 'ucrt64' / 'bin' / 'libopus-0.dll', binaries / 'libopus-0.dll')
    shutil.copy2(make_release.unpack('ninja') / 'ninja.exe', output / 'ninja.exe')
    print('Pinned Engage compiler and dependencies ready')
