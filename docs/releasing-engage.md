# Engage release policy

## First public release: source preview

Publish an explicitly marked prerelease from a reviewed source commit. Its assets may contain the source snapshot, license notices and SHA-256 checksums. It is not a playable game or an Engage setup installer. Preserve upstream license notices and attribution. The README must state the experimental status and distinguish observed gameplay from unverified features.

Before publishing:

1. Run the content guard on the source tree and staged files.
2. Run the Engage source tests and synthetic continuation test.
3. Audit the exact Git revision used to make the source archive with `tools/make_engage_source_release.py`.
4. Confirm that the archive has no private config, game input, generated code, extracted artwork, keys, firmware, saves, caches, logs or locally built game executable.
5. Keep the release tag tied to that commit. Upload only the audited archive and its checksum file. Never zip an existing local build or installation directory.

Source-only GitHub CI intentionally has no game files, keys or firmware. A green source workflow is not a runtime/gameplay certification. Do not upload local gameplay test folders as Actions artifacts.

## Windows setup candidate

Follow TotK's distribution boundary: ship the host runtime/build tools, then extract, recompile and link on the player's computer using their inputs. A complete `engage.exe` is never a public release asset. The user's game icon and fonts are extracted only during local setup.

`engage_setup` supplies the Engage-specific pipeline and form, reusing the MIT-licensed upstream window, event model and cancellation controller. `tools/make_engage_release.py` bundles the pinned Python/compiler dependencies and a self-contained .NET extractor. The inherited `python -m installer` command still targets TotK; Engage uses `python -m engage_setup`.

Build a candidate with Python 3.14:

```powershell
.venv-py314\Scripts\python.exe tools\make_engage_release.py `
  --out build\release\Engage-Setup-<version>-windows `
  --zip build\release\Engage-Setup-<version>-windows.zip
```

Keep the source revision clean for a public build. Never substitute a development install folder for the setup bundle. The builder inventories the result, scans for game-derived content, tests isolated imports and the missing-input command, and opens/closes the setup window. Its content check does not replace a local full-install test.

A setup release also needs a clean-machine end-to-end test, cancellation/retry checks, existing-profile preservation, pinned downloads with hash checks, bundled dependency license texts, artifact inventory and checksums. Test the setup's no-game-files failure path in CI. Keep game-dependent verification local and report its scope honestly.

The current local test uses bundled tools with a minimal PATH and a fresh install folder on D:. This is a developer-machine test, **not a clean-VM certification**. Before calling the release broadly supported, repeat it on a clean Windows machine and record the hardware/driver, install result, first launch, audio, save/load and post-battle behavior. Initial releases must remain marked prerelease.

## Known rendering report

The unit-information panel was reported darker in one screenshot and blue in another. The examples show different states (a unit alone versus paired with Marth). A [normal-state gameplay reference](https://images.igdb.com/igdb/image/upload/t_1080p_2x/sciwxx.webp) also shows a dark panel for Alear without an Emblem partner. This supports a state-dependent UI difference; it does not establish a renderer defect or validate every color. Compare matching unit/state/settings before changing global gamma, blending or shader behavior. No renderer change was made from these screenshots.
