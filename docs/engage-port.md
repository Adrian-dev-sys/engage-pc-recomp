# Fire Emblem Engage Windows port

Status: experimental native Windows build. The user has completed the first battle,
including attack cutscenes, and confirmed audible sound. The opening movie, title and
character preview render. Full-game completion, save/load and long-session stability
remain unverified.
The inherited installer, extraction commands, and default build targets still target TotK. Do not feed Engage
containers to tools/extract.py or use TotK's patch files on Engage.

## Reproducible starting points

- TotK Recomp: https://github.com/valentderah/totk-pc-recomp at
  `f469c5a7745502357fc1664c04e02522fe92c42a` (this checkout's base).
- Engage reference: https://github.com/seedds/nxnative at
  `97f1514a46858b5f48c648022c081b536fbc6ad5`, in ignored `refs/nxnative`.
  Read `dm/games/0100A6301214E000/README.md`, `game.tsv`, and `patches.py`.
- Target: base title `0100A6301214E000` merged with update 2.0.0, Unity
  2020.3.18f1 IL2CPP, SDK 12.3.7. Required lifted modules: main, subsdk0, sdk.
- Keep existing copyright notices and licenses. References are read-only; attribute
  any later copied implementation in THIRD_PARTY_NOTICES.md at its actual revision.

## Same development loop as TotK

1. Keep personal paths in ignored config.local.toml and inputs outside tracked files.
   Extract RAR inputs with 7-Zip: the Windows tar output failed NCA header checks
   in this workspace despite returning success. Build the pinned LibHac extractor:

   ```powershell
   dotnet publish tools/nx_extract/nx_extract.csproj -c Release --artifacts-path build/nx-extract-artifacts -o toolchain/nx_extract --no-self-contained
   python tools/extract_engage.py
   ```

   The ignored config uses input.base, input.update, input.keys, optional
   input.title_keys, and input.dlc mapping each DLC title ID to its NSP path.
   The wrapper checks exact CNMT title IDs/types/versions before extraction. It
   merges through LibHac with integrity failures treated as errors. It requires a
   new ExeFS/RomFS destination, so interrupted output must be inspected before retrying.
   Build the extractor from tools/nx_extract, not the unmodified reference: the local
   version also accepts the title.keys path without logging values. Key-sized hex
   strings are removed from tool logs.
2. Verify inputs and record local hashes:

   ```powershell
   python tools/engage_inputs.py --exefs 'D:\Engage\exefs' --romfs 'D:\Engage\romfs'
   ```

   This checks known module build-ID prefixes and a RomFS marker, not whole-game
   authenticity or integrity. The local inventory stays in ignored build/engage-v200.
3. The initial workspace has Python 3.12.15 and the minimal lifter dependencies in
   .venv. To recreate it without changing the user's global Python installation:

   ```powershell
   $env:UV_PYTHON_INSTALL_DIR = Join-Path (Get-Location) 'toolchain\python'
   uv venv --python 3.12 .venv
   uv pip install --python .venv/Scripts/python.exe -r tools/requirements-engage-lifter.txt
   ```

   Run coverage before attempting execution:

   ```powershell
   .venv\Scripts\python tests/diff/lift.py --exefs 'D:\Engage\exefs' --coverage
   .venv\Scripts\python recomp/gen.py --exefs 'D:\Engage\exefs' --out generated/engage-v200 --no-callee-saved-trust
   ```

   Supply no TotK --patches argument. Generated C is not a working port. Never edit
   it by hand. If the lifter changes, run the upstream differential tests, including
   SIMD, atomics, functions and jump tables, as required by AGENTS.md.
4. Port the runtime boundaries before linking an Engage executable. Audit nn SDK
   imports, exception unwinding, GC thread suspension, virtual address allocation,
   AudioOut/Wwise, movie decoder, fonts, keyboard and applet handling against nxnative.
   Phone memory limits are research, not mandatory Windows settings.
5. Adapt shader collection to UnityFS, data.unity3d and Addressables. Verify separate
   textures/samplers, depth mode, CPU-written movie textures, and pool destruction
   against Vulkan. TotK's shader census and prewarm list do not cover Engage.
6. Add an Engage build target and remove TotK-specific runtime assumptions. Then run
   selftests and kernel/graphics probes before booting, and resolve the first concrete
   failure from logs. Add frame-rate patches only after baseline timing works, guarded
   by both build IDs and expected instruction bytes.
7. Playtest title, name entry, prologue battle, hub, movies, saves/reloads and extended
   sessions. Claim only milestones actually observed. Run tools/check_content.py
   before commits; all generated content stays under build/ or generated/.

## Current checks and blockers

The input verifier and extractor have synthetic tests:
`python tests/engage_inputs_test.py` and `python tests/extract_engage_test.py`.

Observed on the supplied files:

- Base Application `0100A6301214E000` v0 and Patch `0100A6301214E800` v327680
  have the expected content metadata. The control data reports 2.0.0.
- DLC Wave 1 `0100A6301214F001` v0 is extracted to
  `build/extracted/engage-v200/aoc/1/e0` (empty marker).
- Order of Heroes Item Set `0100A6301214F002` v0 is extracted to
  `build/extracted/engage-v200/aoc/2/h0` (empty marker).
- Both DLC extractions completed with LibHac integrity checking enabled. As nxnative
  documents, their main content is in update 2.0.0. Do not synthesize markers when
  a package is missing. The Engage runtime enumerates and mounts the two extracted
  directories read-only. The filesystem probe verifies both markers and rejects writes
  and traversal. In-game DLC activation remains untested.
- The replacement multipart update supplied later contains a ticket matching its
  Program NCA. Extraction of base + update now succeeds with integrity checking:
  five ExeFS files and 206,874 RomFS files (15,804.7 MiB). All three module build IDs
  match the expected 2.0.0 profile. Both DLCs were extracted again successfully, and
  the combined local inventory is `build/engage-v200/inputs.json`.
- All six shared fonts extracted from firmware 22.5.0 with verification passing.
- Instruction coverage completed in 12 seconds. Rejected words were classified
  only as UDF or undecodable encodings in the upstream census; this does not establish
  reachable-code correctness. Results: `build/engage-v200/coverage.log`.
- C generation completed in 273 seconds without patches and without trusting
  callee-saved registers: 312,139 main functions, 13,689 subsdk0 functions and 38,708
  sdk functions (364,536 total), in 464 chunks, about 1,460 MB of C. The generator
  reported zero unsupported instructions, but emitted 153,471 trap sites for rejected
  UDF/undecodable words; reachability and correctness still need review and tests.
  Results and final statistics: `build/engage-v200/lift.log` and each
  `generated/engage-v200/<module>/manifest.json`.
- A static import comparison is recorded in `build/engage-v200/symbol-audit.json`.
  Its five unresolved names are all weak imports; the inherited loader allows weak
  imports to resolve to zero. The scan ignores macro-generated overrides and is not
  a runtime compatibility result.
- Native build: `build/engage-v200/engage.exe`, about 322 MiB. The local, hash-pinned
  llvm-mingw toolchain and libraries come from tools/make_release.py's existing pins.
  No global MSYS2 installation is required for this build.
- Assets: `build/extracted/engage-v200/packed-romfs/romfs.bin`, 16,572,416,159 bytes;
  its 206,874-file index is checked by the runtime. Packing uses the verified expanded
  input tree and does not replace it.
- Loader: Engage has its own identity and measured constructor counts. Default-visibility
  definitions bind globally in load order, so SDK C++ deletes use Unity's allocator in
  main. Eight allocator bindings are checked when loading. The old own-definition-first
  behavior hit CentralHeap::Free's debug break on Unity allocations.
- GC: cooperative SetThreadActivity/GetThreadContext3 support pauses threads at published
  Cpu boundaries and returns the 0x320-byte Horizon context. Engage disables asynchronous
  core preemption so snapshots do not read stale lifted locals. The kernel probe verifies
  20 pause/context/resume cycles across running and sleeping threads, plus invalid inputs.
  This is not long-session GC validation; a thread that cannot reach a boundary in one
  second returns an error rather than falsely claiming it was paused.
- Boot: all constructors complete, nnMain runs, and `--boot` reaches the first
  nvnBootstrapLoader call with exit 0. Basic networking initialization is local/offline;
  online services and later socket operations are not implemented.
- Rendering: a Vulkan run presented initial black frames. A bounded 0.05-second capture
  exited 0 after two frames with zero reported shader failures or validation errors.
  This is not a title-screen or gameplay milestone. `first-frame.png` is black.
- Shader extraction: the pinned nxnative Unity reader found 9,280 shader stages
  (4,637 vertex, 4,641 pixel, one compute, one geometry). The local converter makes
  9,279 VS/PS/CS probe jobs; the geometry record is retained but not part of the probes.
  The reader warns that some built-in ComputeShader objects have no type tree, so the
  survey must not be called complete. The 32-sample translator/cache probe passes.
  The Vulkan execution probe also passes its graphics, compute, transfer, indirect,
  NVN extension, zero-memory and mirror-readback checks (`build/engage-v200/vkexec.log`).

## Build and verify this milestone

With the existing verified extraction and generated C:

```powershell
.venv-py314\Scripts\python tools/setup_engage_toolchain.py
.venv\Scripts\python tools/build_xlate.py
python tools/pack_engage_romfs.py --source build/extracted/engage-v200/RomFS --out build/extracted/engage-v200/packed-romfs
python tools/build_engage.py --jobs 12
build\engage-v200\engage.exe --selftest
build\engage-v200\engage.exe --probe-kernel
build\engage-v200\engage.exe --probe-engage-fs
build\engage-v200\engage.exe --boot
```

Skip packing when its completed image/index already exist; the packer rejects overwrites.
The build is incremental. No generated C or game-derived data is committed. Existing
runtime setting names (TOTK_* and totk.ini) are retained during bring-up.

For the Unity shader survey, run the reference tool with `DM_GAME=0100A6301214E000`,
`PYTHONUTF8=1`, and `PYTHONDONTWRITEBYTECODE=1`, passing the verified RomFS, ExeFS/main,
and output package/map/facts/pair paths under build/engage-v200. Then run
`.venv-py314\Scripts\python tools/engage_shader_census.py`. UTF-8 mode is needed on
Windows for the source map's header. The converter retains source associations so
the Vulkan probe can select vertex/pixel candidates from the same source.

## Baseline exception failure

The first observed managed exception is `FileNotFoundException` for
`UnityEngine.UIElementsModule, Version=0.0.0.0, Culture=neutral, PublicKeyToken=null`.
This may be a normal optional-assembly probe. Do not infer an incomplete game dump
or replace the assembly with invented content just from that exception.

The exception path initially crashed while GetExceptionInfo walked a module gap
reported as executable. Engage now reports unmapped gaps as free/no permissions.
It then reaches a bad indirect call during SDK termination, guest PC
`0xd503201f143f6127`, LR `sdk+0x5120a8`. Logs: `build/engage-v200/unwind.log`.
The observer logs the managed type and strings without replacing the guest unwind ABI.

The opt-in continuation build below gets past this failure without suppressing the
exceptions. The preserved baseline still demonstrates the original crash.


## Experimental exception continuations

The baseline executable is preserved as `build/engage-v200/engage-boot-baseline.exe`.
The opt-in generator flag `--continuations` reads CIE/FDE/LSDA exception metadata
and emits resume labels for landing pads and call return addresses. It keeps guest
registers and flags in Cpu so a resumed edge cannot bypass local initialization.
A guest RET with a changed return address or stack position transfers through the
nearest surviving host-to-guest boundary. Continuation dispatch validates its
address in the generated containing function; ordinary dispatch still requires
function starts. TotK keeps its original rt_call path.

The synthetic native test uses the real dispatcher, compares the landing-pad result
against Unicorn, and also checks a transfer across a nested host callback. Four
focused tests pass. Both default and continuation function emission pass 137 cases
on 87 functions. Additional samples pass: CPU 1,534 cases; SIMD 512 cases; atomics
396 cases. These samples do not establish whole-game correctness or gameplay.

```powershell
.venv\Scripts\python recomp/gen.py --exefs build/extracted/engage-v200/ExeFS --out generated/engage-v200-cont --continuations --no-callee-saved-trust --jobs 4
.venv\Scripts\python tools/build_engage.py --gen generated/engage-v200-cont --out build/engage-v200-cont --jobs 12
.venv\Scripts\python -m unittest discover -s tests -p continuations_test.py
```

The experimental executable passes --selftest, --boot, --probe-kernel,
--probe-engage-fs, --probe-nvn and --probe-engage-input. The real startup now catches
the optional assembly exceptions and continues to both DLC mounts, local account/save,
audio and controller initialization. Offline prepo initialization uses the existing
local reporting policy; all five singular SDK 12 GetNpadState overloads use the same
state backend as GetNpadStates. The input probe checks connection styles and output bounds.

A one-second game-clock capture exited 0 after 28 frames in about 6.1 wall seconds,
but the image is black (`build/engage-v200-cont/debug-startup.png`). A longer run
hit a native access violation in the `vk recorder` thread, inside nvoglv64.dll.
Logs: `build/engage-v200-cont/startup-3.log` and `debug-10.log`. Vulkan validation
is not installed: zero reported validation errors in prior runs does not establish
API validation coverage. The next investigation is the exact failing recorded command.

Host callbacks must not
hold resources that require cleanup across a guest nonlocal return; the synthetic
check covers plain callback frames, not every host subsystem.


## First visible rendering

Unity frees each shader control section after nvnProgramSetShaders. The runtime now
owns Engage's 0x880-byte control copies until replacement/finalization, with the
backend drained before freeing. The NVN probe passes 803 checks, including six-stage
caller-overwrite, replacement, invalid count, empty set and finalization checks.
This matches the lifetime issue documented in the pinned nxnative Engage README;
no reference implementation was copied.

The frame-300 dump then identified a second issue: sampled textures all reflected
as slot -1 and therefore used dummy black images. The translator's packed NVN
texture/sampler handle is 32 bits and independent of its SPIR-V separate-descriptor
flag. Reflection now preserves that handle, decodes its texture and sampler slots,
and the descriptor builder combines the separately bound NVN handles. Descriptor
reuse also tracks the sampler binding. The synthetic reflection/binding checks,
32-shader translation/cache probe and full Vulkan execution probe pass.

`bindings-startup.log`: 351 frames, exit 0, no reported shader failures or skipped
draws; 12 seconds of the game clock, about 19.7 wall seconds. `bindings.png` visibly
shows the opening movie at 1920x1080. These tests use `TOTK_VK_RECORDER=0`; this is
an experimental setting, not proof the earlier recorder crash is fixed. The previous
black captures are retained for comparison. Both docked and handheld input probes pass.


## Title-screen milestone

`title-attempt.log` records a scripted PLUS press at game-clock 15 seconds to skip
the opening movie. The 25-second run completed 739 frames and exited 0, with zero
reported shader failures or skipped draws. `title-attempt.png` visibly shows the
Fire Emblem Engage title, Press Any Button, and Ver. 2.0.0 at 1920x1080. Both DLCs
mounted during that run. This verifies the title screen, not menus, new-game flow,
combat, save/load or DLC gameplay.

`Run-Engage.cmd` launches the experimental executable with the tested Vulkan,
recorder-off and shared-font settings. It supplies no scripted button presses and
forwards optional command-line arguments. It uses the existing local Engage profile;
the original baseline executable and its generated C remain available for comparison.


## Character-preview rendering

The user's Appearance screen drew its UI but no character. A setup-frame dump showed
that additional material textures still resolved to slot -1: their packed handles
put the sampler offset before the texture offset. Texture/sampler OR operands have
no fixed order. Both gfx_shader.c add_res and the translator Accessor.TexSlot now
recognize either ordering, including the independently encoded buffer slots.
Reflection checks cover texture-first, sampler-first, distinct texture/sampler
slots, and the original combined-handle path. Translator identity is now
`d459a968d917e277`.

The updated 32-shader/cache probe and full Vulkan execution probe pass. A real
50-second setup run exited 0 after 1,488 frames and showed the character behind
name confirmation (`appearance-reversed.png`). The following 48-second run exited
0 after 1,428 frames; `appearance-screen.png` shows the character clearly in network
configuration. No reported shader failures or skipped draws. Tests used the separate
`build/engage-v200-cont/repro-appearance-profile`; user-profile saves were not used.
Online play remains unimplemented; choose Unconnected in the setup dialog. Combat,
save/load and long-session stability still need testing.


## First-battle lifecycle crash

The user reported a crash just before combat. The preserved log
`build/engage-v200-cont/user-before-combat-crash.log` shows a null read in
sdk+0x33a110, EnterExitRequestHandlingSection (LR sdk+0x33a138), after frame 4800.
The SDK routine obtains a console applet interface that this host environment does
not initialize. Both EnterExitRequestHandlingSection and its matching Leave now
use a mutex-protected local nesting counter; overflow and unmatched leave remain
fatal. Host process shutdown remains owned by the existing window/runtime path.

The input/system probe verifies eight nested enter/leave calls through the actual
registered overrides. It, --selftest, --boot and --probe-kernel pass after rebuilding.
This fixes the identified null-access path; the complete first-battle transition
still needs a fresh gameplay test. No user saves were modified while diagnosing it.


## First-battle offline communication check

The next user run got past EnterExitRequestHandlingSection and stopped on the
unimplemented `pctl` service in IsFreeCommunicationAvailable. The preserved log is
`build/engage-v200-cont/user-before-combat-pctl.log`.

All three free-communication APIs imported by main now have host implementations:
IsFreeCommunicationAvailable and TryBeginFreeCommunication(bool) return false;
EndFreeCommunication has no online session to release. This represents the port's
offline state and does not open a console permission dialog or change host settings.
The probe checks both show-UI argument values, end-before-availability queries, and
false results. The extended system/input probe and boot check pass after rebuilding.
The complete first battle remains unverified until another gameplay run.


## AudioOut and the sound-bank directory

Wwise initialized successfully but immediately terminated because GetEntryType on
`rom:/Data/StreamingAssets/Audio/GeneratedSoundBanks/Switch/` returned 0x202. The RomFS
lookup appended a second trailing slash. It now preserves an existing trailing slash;
regression checks cover the directory with and without it, the bank file, and rejection
of duplicate interior separators. The Engage filesystem probe passes.

Once that check passed, Wwise reached `audout:u`, which this runtime previously lacked.
`runtime/audout.c` now provides the AudioOut manager and PCM16 output queues using SDL3.
Each output owns its buffers, volume, clock, release event and played-frame counter.
Silent-clock fallback preserves progress without a playback device. Queue validation
rejects duplicate tags, over-capacity offsets, misaligned PCM and oversized buffers.
The SDK's single-buffer release accessor expects an empty response slot to contain
zero, so release replies clear unused slots as well as returning the count.

`--probe-audout` checks partial consumption, sample order, stopped output, duplicate
submission, invalid offsets, release ownership, empty reply clearing and flush. It
runs without a speaker. The 25-second `audio-banks-flow.log` run exited 0, and its SDL
path reported nonzero playback samples; the user independently confirmed hearing audio.
Temporary game-address wrappers and SDK tracing were removed from the final build.

## Post-battle account check

After the first completed battle, `user-after-first-battle-account.log` identified a
crash in IsNetworkServiceAccountAvailable. Its console SDK implementation treated the
local host UserHandle token as a console interface pointer. The host implementation
validates that handle and reports a successful offline result (availability false).
The system/input probe checks valid/closed handles, output bounds and null outputs.
The actual post-battle transition still needs another player test.

## Engage branding

The window, settings version label and Windows version details identify Engage PC Recomp.
The Windows icon is generated in the ignored build directory from the user's extracted
`icon.jpg`, with seven sizes from 16 to 256 pixels. Neither the game artwork nor the
resulting icon is tracked. The Engage build emits its own resource file and retains
the existing UTF-8 and DPI manifest. Settings and rotating logs use `engage.ini` and
`engage.log`; the build preserves an existing `totk.ini` by copying it to `engage.ini`
only when the latter does not exist. The existing Run-Engage.cmd launcher still applies.
Shared TotK engine source names and environment-variable interfaces remain compatible.


Final branding verification showed the live window titled Engage PC Recomp with the
Engage icon in its title bar and taskbar. The F1 menu showed Engage PC Recomp 0.1.0,
game 2.0.0. `audio-branded-final.log` completed 25 seconds with nonzero SDL playback
samples and no reported shader failures. Audio, Engage filesystem, input/system,
configuration and runtime self-test probes pass. Full-game and long-session checks
remain open.

## Feature docs

- [engage-graphics.md](engage-graphics.md): anti-aliasing, DLSS / FSR upscaling modes.
- [engage-mods.md](engage-mods.md): mods and Cobalt mod support.
- [engage-qol.md](engage-qol.md): PC quality of life.
