# Engage PC Recomp

Experimental native Windows x86-64 recompilation of **Fire Emblem Engage 2.0.0**, built on [TotK Recomp](https://github.com/valentderah/totk-pc-recomp).

[![Source checks](https://github.com/veneito/engage-pc-recomp/actions/workflows/content-guard.yml/badge.svg)](https://github.com/veneito/engage-pc-recomp/actions/workflows/content-guard.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

**Early alpha. The Windows setup pipeline passed a local installation test; no installer asset has been published yet.** First-battle gameplay and audio work in the tested development build. Full-game compatibility is still being established.

[Installation guide](docs/installing-engage.md) · [Report a bug](https://github.com/veneito/engage-pc-recomp/issues/new/choose) · [Build from source](docs/building-engage.md)

## Game files stay on your computer

This repository contains the port's source and tools, not the game. Supply your own base game, update 2.0.0, keys and supported DLC dumps. Build locally. Do not upload game containers, keys, firmware, generated game code, a built `engage.exe`, extracted icons, shader caches or save files to this repository or its issues. A locally compiled game executable contains game code and is not a redistributable release asset.

## Current status

- Opening movie, title screen and character preview render.
- A player completed the first battle, including attack cutscenes.
- Audio playback works through the Windows output device.
- Keyboard/gamepad input and the F1 settings menu work in the tested build.
- The two supported DLC packages are extracted and mounted; DLC gameplay is unverified.

Known limits: save/load, the post-battle transition and long-session stability still need testing. Rendering remains experimental. The reported dark-versus-blue unit-information panel also appears in gameplay references with different Emblem-pairing states; a matching-state rendering fault has not been established. Vulkan validation coverage is not established. The tested configuration disables the Vulkan recorder.

The current evidence is from a Windows 11 machine with an NVIDIA RTX 4060. This is not a claim of compatibility with every GPU or of full-game completion. Linux, Steam Deck, performance patches and inherited TotK features are not verified for Engage.

## Installing

The setup follows the same local-build approach as [TotK Recomp](https://github.com/valentderah/totk-pc-recomp): download setup, select your files, install, and play. Python, the compiler and the extractor are bundled. Your game is compiled on your PC.

1. Extract the **Engage-Setup Windows ZIP** when it becomes available in Releases.
2. Run **Engage-Setup.exe** and select your base game, **2.0.0** update, keys and firmware folder. Either DLC is optional.
3. Choose an install folder with **40 GiB free**, then select **Install Engage**.
4. When setup finishes, select **Play Engage**. Press **F1** for settings.

![Engage Setup — choose your files, install, and play](docs/screenshots/setup.png)

See the [installation guide](docs/installing-engage.md) for requirements and help with update/key errors. This is a Windows alpha; the inherited TotK installer and its game-specific patches do not install Engage.

## Building from source

Follow [the Engage build guide](docs/building-engage.md). Building directly from source requires developer tools. A ready-to-play game executable is never distributed.

Launch a completed local build with `Run-Engage.cmd`. Settings use `engage.ini`; logs use `logs/engage.log` beside the executable. Your profile stays in the ignored `build/profile-engage` directory.

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md). Report reproducible bugs with the port/game version, GPU/driver and the exact scene. Review logs for personal paths before attaching them. Never attach game data, keys, firmware, saves or locally compiled game executables.

If this project is useful to you, a GitHub star helps others find it. Reproducible bug reports and contributions help make it better.

[Porting notes and observed results](docs/engage-port.md) distinguish tested behavior from open work. [Release policy](docs/releasing-engage.md) describes what may be published and what must be verified before an installer release.

## Credits and license

This is a fork of **TotK Recomp** by Valiantsin Dzerakh, based on commit `f469c5a7745502357fc1664c04e02522fe92c42a`. The original project's MIT license and contributor notices are preserved. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for the upstream components and their licenses, including LAS Recomp, MacNativeGames, SavageWindRecomp, nxnative, Ryujinx, SDL, Dear ImGui, libopus, libvpx and Zstandard.

The port's code is under the [MIT License](LICENSE); third-party components retain their own licenses. This license does not cover Nintendo game content. Engage PC Recomp is an independent fan project, not affiliated with or endorsed by Nintendo or Intelligent Systems. Fire Emblem and Nintendo Switch are their respective owners' trademarks.

Inherited TotK documentation is retained for engine history and attribution in [docs/upstream-readme.md](docs/upstream-readme.md); its installation and game-specific claims do not apply to Engage.
