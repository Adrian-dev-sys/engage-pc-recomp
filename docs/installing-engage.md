# Installing Engage PC Recomp

This fork has no releases, so there is no setup ZIP to download. After the toolchain steps of [the build guide](building-engage.md), start the same setup form from the repository with `.venv-py314\Scripts\python.exe -m engage_setup`, or build a setup package yourself with `tools/make_engage_release.py` ([release notes](releasing-engage.md)). The steps below describe that form.

## What you need

| Requirement | Details |
| --- | --- |
| Windows | Windows 10 or 11, 64-bit |
| CPU | x86-64-v3, including AVX2 |
| Graphics | Vulkan 1.3-capable GPU with a current driver |
| Storage | At least 40 GiB free on the installation drive, in addition to your input files |
| Game | Your base Engage NSP or XCI and the cumulative **2.0.0** update NSP |
| Keys | Your console's `prod.keys`; `title.keys` if your update needs it |
| Firmware | Your extracted firmware folder containing NCA files, for the shared fonts |
| Optional DLC | Expansion Pass and/or Order of Heroes Item Set, as separate NSPs |

Run from the repository, setup uses the Python, compiler and extractor the build guide sets up. Game files, keys and firmware are supplied by you and stay on your computer.

## Install

![Engage Setup with required files grouped above the optional DLC and title-key inputs](screenshots/setup.png)

1. Start the setup form as described above.
2. Choose your base game, update **2.0.0**, `prod.keys` and extracted firmware folder. Add either DLC and `title.keys` if needed.
3. Choose a separate, empty installation folder, such as `D:\Games\Engage`. Select **Install Engage**. Setup checks your hardware, verifies and extracts the game, recompiles it locally, and installs it. Allow 20–60 minutes initially; time varies by CPU and storage.
4. Select **Play Engage**. Later, open **Play Engage.cmd** in your installation folder. Press **F1** for settings and controls.

Your original dumps are unchanged. The installed executable contains locally generated game code: keep the installation private. The installed game icon and fonts come from your inputs, not from this repository.

## If setup stops

- **Missing file:** select the actual file after extracting any outer archive. A firmware ZIP must be extracted into an NCA folder first.
- **Wrong update:** this port targets Engage **2.0.0**, title ID `0100A6301214E800`. Other game versions are rejected.
- **Ticket or title-key error:** firmware keys do not replace a game's title key. Use a valid dump from your console; optionally select your `title.keys`. Never paste keys into an issue.
- **No Vulkan 1.3 GPU:** install the current graphics driver for your GPU and retry.
- **Not enough space:** choose a drive with at least 40 GiB free. Temporary extraction and compilation stay on that drive.
- **Interrupted installation:** rerun the same setup with the same inputs and destination. Only completed steps are checkpointed. If setup reports missing or inconsistent work, use a fresh folder and keep the original installation's profile.

The progress window offers a log view. Errors include the setup log's location. Review personal paths before sharing a log. Do not attach game containers, extracted files, keys, firmware, generated code, saves or compiled game executables.

## Playing and keeping saves

Settings are in `engage.ini`; the player profile is in `profile`, both inside the chosen installation folder. Back up `profile` before experimenting. Setup does not overwrite an existing profile or settings file.

Shader caches populate during play, so first-time effects may pause briefly. This alpha has been tested through the first battle with audio and attack cutscenes; save/load, post-battle transitions, full-game completion and long sessions remain unverified. See the [current status](../README.md#current-status) before reporting a problem.

## Updating this alpha

The candidate supports resuming the **same** setup build. In-place upgrades between setup versions are not implemented yet. Install a newer version into a fresh folder and keep your existing profile; do not delete your old installation until your saves have been checked in the new one.
