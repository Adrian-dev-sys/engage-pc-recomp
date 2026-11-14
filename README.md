# Engage PC Recomp

Experimental native Windows x86-64 recompilation of **Fire Emblem Engage 2.0.0**, built on [TotK Recomp](https://github.com/valentderah/totk-pc-recomp).

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

## About this fork

This is a personal fork of [Engage PC Recomp](https://github.com/xgc1986/engage-pc-recomp). It has no releases and no
ready-made installer download: build it from source as described below. Most of its changes were written with an AI
assistant (Claude) and tested by hand on one Windows PC with an NVIDIA GPU. Its additions include:

- DLSS anti-aliasing and upscaling modes (Quality to Ultra Performance), with AMD FSR 3.1 as the fallback.
- Shadow resolution, render-target upscaling of menu models, ultrawide fixes (Wyvern Ride reticle and more).
- Game speed fixes above 30 fps (cameras, animations), a larger memory pool for heavy mods.
- Loading of mods made for [Cobalt](https://github.com/Raytwo/Cobalt): outfits, XML and text patches, sound banks,
  voices, Lua scripts, supports and custom sprites.
- DualSense rumble and gyro aiming.

The DLSS and FSR libraries are not included. Put NVIDIA's `nvngx_dlss.dll` (from the
[DLSS SDK](https://github.com/NVIDIA/DLSS)) and AMD's `amd_fidelityfx_vk.dll` (from the
[FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)) next to `engage.exe` to use them.

## Game files stay on your computer

This repository contains the port's source and tools, not the game. Supply your own base game, update 2.0.0, keys,
firmware and DLC dumps, and build locally. Do not upload game containers, keys, firmware, generated game code, a built
`engage.exe`, extracted icons, shader caches or save files to this repository or its issues. A locally compiled game
executable contains game code and must not be redistributed.

## Current status

On the tested PC (Windows 11, NVIDIA GPU):

- Battles, attack cutscenes, the Somniel and its minigames, Wyvern Ride and both DLC packages play.
- Audio, keyboard, gamepad and DualSense (rumble, gyro) work. Press **F1** for the settings menu.
- Ultrawide, frame rates above 30 fps and Cobalt-format mods work, with the exceptions below.

Known limits: long play sessions are not yet tested. The combat cannon does not rotate correctly, and some minigame
timing is off above 30 fps. HDR output is not supported. FSR 3.1 looks grainier than DLSS. Other GPUs, Linux, macOS
and Steam Deck are untested.

## Building from source

Follow [the Engage build guide](docs/building-engage.md). It needs developer tools (MSYS2 Clang, Python, .NET) and your
own game dumps. The setup form in `engage_setup/` (`python -m engage_setup`) drives the same pipeline; see [the installation guide](docs/installing-engage.md)
for the files it asks for and help with update and key errors.

Launch a completed local build with `Run-Engage.cmd`. Settings use `engage.ini`; logs use `logs/engage.log` beside the
executable. Your profile stays in the ignored `build/profile-engage` directory.

More detail: [porting notes](docs/engage-port.md), [graphics options](docs/engage-graphics.md),
[mod loading](docs/engage-mods.md), [quality-of-life changes](docs/engage-qol.md), [controls](docs/controls.md).

## Credits and license

- The [Engage PC Recomp](https://github.com/xgc1986/engage-pc-recomp) authors, whose port this fork builds on.
- **TotK Recomp** by Valiantsin Dzerakh and its contributors, based on commit `f469c5a7745502357fc1664c04e02522fe92c42a`.
  The original project's MIT license and contributor notices are preserved.
- Raytwo and the Cobalt contributors, whose mod format this fork loads (written anew, no Cobalt code included).
- AMD for the FidelityFX API headers (MIT).
- Every project listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), including LAS Recomp, MacNativeGames,
  SavageWindRecomp, nxnative, Ryujinx, SDL, Dear ImGui, libopus, libvpx and Zstandard.

The port's code is under the [MIT License](LICENSE); third-party components keep their own licenses. This license
does not cover Nintendo game content. Engage PC Recomp is an independent fan project, not affiliated with or endorsed
by Nintendo or Intelligent Systems. Fire Emblem and Nintendo Switch are their respective owners' trademarks.

Inherited TotK documentation is kept for engine history and attribution in [docs/upstream-readme.md](docs/upstream-readme.md);
its installation and game-specific claims do not apply to Engage.
