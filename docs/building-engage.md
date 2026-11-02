# Building Engage PC Recomp

This is a developer source preview for Windows x86-64. Use Engage's commands below, not the inherited TotK installer or patch lists. Keep the checkout in a writable folder with ample disk space: both extracted and packed RomFS occupy roughly 16 GB, in addition to game containers, tools, generated C and objects.

Prerequisites: Git, Python 3.12 and 3.14, .NET 10 SDK, Visual Studio C++ build tools (for the shader translator's NativeAOT link), and Windows PowerShell. The game requires an AVX2-capable CPU and a compatible Vulkan GPU; the tested machine has an RTX 4060. These are not minimum-performance guarantees.

## Prepare tools

From the repository root in PowerShell:

```powershell
py -3.12 -m venv .venv
.venv/Scripts/python -m pip install -r tools/requirements-engage-lifter.txt
py -3.14 -m venv .venv-py314
.venv-py314/Scripts/python -m pip install -r tools/requirements-installer.txt
.venv-py314/Scripts/python tools/setup_engage_toolchain.py

dotnet publish tools/nx_extract/nx_extract.csproj -c Release --artifacts-path build/nx-extract-artifacts -o toolchain/nx_extract --no-self-contained
.venv/Scripts/python tools/build_xlate.py
```

Compiler/runtime downloads are pinned and verified by the inherited release tooling. The extractor uses pinned LibHac 0.19.0. No compiler, extractor or dependency executable is taken from a user's local game build.

## Supply your own inputs

Copy `config.engage.example.toml` to `config.local.toml`. Enter local paths for your base NSP, update 2.0.0 NSP, keys, firmware NCA directory and both supported DLC packages. The current extraction workflow requires both DLCs; installing without them is not yet a supported setup path. An update package must include a valid ticket or be usable with your own matching title key. This project does not supply keys, firmware, tickets or game downloads.

```powershell
.venv-py314/Scripts/python tools/extract_engage.py
.venv-py314/Scripts/python tools/extract_fonts.py
.venv/Scripts/python tools/pack_engage_romfs.py --source build/extracted/engage-v200/RomFS --out build/extracted/engage-v200/packed-romfs
```

Extraction verifies package identity, the target update and content integrity. Packing refuses to overwrite an existing output. Inspect an interrupted setup before retrying; do not delete your input dumps or profile.

## Recompile locally

```powershell
.venv/Scripts/python recomp/gen.py --exefs build/extracted/engage-v200/ExeFS --out generated/engage-v200-cont --no-callee-saved-trust --continuations
.venv/Scripts/python tools/build_engage.py --gen generated/engage-v200-cont --out build/engage-v200-cont --jobs 12
.\Run-Engage.cmd
```

Adjust compile jobs to your machine's available memory. Generation and the first compilation take time. Keep the generated C private. The build generates the Windows icon locally from the extracted game icon; neither icon nor compiled game code belongs in a public archive.

The launcher selects Vulkan, disables the experimental recorder and locates your extracted shared fonts. F1 opens settings, F10 changes the performance overlay and F11 changes fullscreen. Preserve `build/profile-engage` when rebuilding.

## Check changes

```powershell
.venv/Scripts/python tools/check_content.py
.venv/Scripts/python -m unittest discover -s tests -p "engage*_test.py"
.venv/Scripts/python tests/extract_engage_test.py
.venv/Scripts/python tests/continuations_test.py
build/engage-v200-cont/engage.exe --selftest
build/engage-v200-cont/engage.exe --probe-audout
build/engage-v200-cont/engage.exe --probe-engage-fs
build/engage-v200-cont/engage.exe --probe-engage-input
```

Lifter changes also require the differential checks in the inherited [engine build guide](building.md). Passing source-only CI does not establish game playability. See [porting notes](engage-port.md) for observed results and limits.
