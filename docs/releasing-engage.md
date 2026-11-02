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

## Later Windows setup release

Follow TotK's distribution boundary: ship the host runtime/build tools, then extract, recompile and link on the player's computer using their inputs. A complete `engage.exe` is never a public release asset. The user's game icon and fonts are extracted only during local setup.

The inherited TotK setup GUI and `tools/make_release.py` are not Engage installers. Before advertising an Engage installer, finish and test its Engage-specific input selection, base/update merge, both DLC mounts, shared-font extraction, continuation-enabled generation, shader translator, local build, launcher and profile handling.

A setup release also needs a clean-machine end-to-end test, cancellation/retry checks, existing-profile preservation, pinned downloads with hash checks, bundled dependency license texts, artifact inventory and checksums. Test the setup's no-game-files failure path in CI. Keep game-dependent verification local and report its scope honestly.

## Known rendering report

The unit-information panel was reported darker in one screenshot and blue in another. The examples show different states (a unit alone versus paired with Marth); the cause is not established. Compare matching unit/state/settings before changing global gamma, blending or shader behavior. Track this as an investigation, not a confirmed fixed bug or a full rendering validation.
