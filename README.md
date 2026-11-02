# Engage PC Recomp

Experimental native Windows x86-64 recompilation of **Fire Emblem Engage 2.0.0**, built on [TotK Recomp](https://github.com/valentderah/totk-pc-recomp).

**Source preview. There is no supported Engage installer release yet.** The inherited TotK installer does not install Engage. Do not use TotK setup packages or game patches for this project.

## Game files stay on your computer

This repository contains the port's source and tools, not the game. Supply your own base game, update 2.0.0, keys and supported DLC dumps. Build locally. Do not upload game containers, keys, firmware, generated game code, a built `engage.exe`, extracted icons, shader caches or save files to this repository or its issues. A locally compiled game executable contains game code and is not a redistributable release asset.

## Current status

- Opening movie, title screen and character preview render.
- A player completed the first battle, including attack cutscenes.
- Audio playback works through the Windows output device.
- Keyboard/gamepad input and the F1 settings menu work in the tested build.
- The two supported DLC packages are extracted and mounted; DLC gameplay is unverified.

Known limits: save/load, the post-battle transition and long-session stability still need testing. Rendering differences remain under investigation, including the unit-information panel's color in different states. Vulkan validation coverage is not established. The tested configuration disables the Vulkan recorder.

The current evidence is from a Windows 11 machine with an NVIDIA RTX 4060. This is not a claim of compatibility with every GPU or of full-game completion. Linux, Steam Deck, performance patches and inherited TotK features are not verified for Engage.

## Build and run

Follow [the Engage build guide](docs/building-engage.md). It requires developer tools and a local build; there is no ready-to-play executable to download.

Launch a completed local build with `Run-Engage.cmd`. Settings use `engage.ini`; logs use `logs/engage.log` beside the executable. Your profile stays in the ignored `build/profile-engage` directory.

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md). Report reproducible bugs with the port/game version, GPU/driver and the exact scene. Review logs for personal paths before attaching them. Never attach game data, keys, firmware, saves or locally compiled game executables.

[Porting notes and observed results](docs/engage-port.md) distinguish tested behavior from open work. [Release policy](docs/releasing-engage.md) describes what may be published and what must be verified before an installer release.

## Credits and license

This is a fork of **TotK Recomp** by Valiantsin Dzerakh, based on commit `f469c5a7745502357fc1664c04e02522fe92c42a`. The original project's MIT license and contributor notices are preserved. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for the upstream components and their licenses, including LAS Recomp, MacNativeGames, SavageWindRecomp, nxnative, Ryujinx, SDL, Dear ImGui, libopus, libvpx and Zstandard.

The port's code is under the [MIT License](LICENSE); third-party components retain their own licenses. This license does not cover Nintendo game content. Engage PC Recomp is an independent fan project, not affiliated with or endorsed by Nintendo or Intelligent Systems. Fire Emblem and Nintendo Switch are their respective owners' trademarks.

Inherited TotK documentation is retained for engine history and attribution in [docs/upstream-readme.md](docs/upstream-readme.md); its installation and game-specific claims do not apply to Engage.
