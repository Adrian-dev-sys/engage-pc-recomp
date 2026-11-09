# Mods (Engage)

Put each mod in its own folder under `mods\` next to `engage.exe` (`TOTK_MODS_DIR` overrides). The layouts Ryujinx,
Yuzu and Atmosphere use all work:

- `mods\<name>\romfs\...`
- `mods\<name>\atmosphere\contents\0100A6301214E000\romfs\...` (any case of the title id)
- `mods\<name>\contents\0100A6301214E000\romfs\...`, `mods\<name>\0100A6301214E000\romfs\...`

- Cobalt mods (GameBanana, `sd:/engage/mods/<mod>` on the Switch): the mod folder as it is, e.g.
  `mods\Seaside Dragon\config.yaml`, `...\Data\StreamingAssets\...`, `...\patches\xml\Item.xml`. See below.

Files in a mod's `romfs` replace the game's files with the same path (matched ignoring case) or are added. F1 > Mods
lists the mods: A / click switches one on or off, the top / left face button (Ctrl+Up / Ctrl+Down) moves it. A mod
higher in the list wins a file two mods replace. Changes apply after a restart. The order and switches are stored in
`engage.ini` `[mods]` as `mods_order` and `mods_disabled` (names separated by `|`).

Code mods (an `exefs` folder with `.ips` / `.pchtxt` files or `subsdk` modules, or `atmosphere\exefs_patches`) cannot
be applied to the recompiled game; such a mod is listed and stays off. The usual ones are built in: frame rate (Game),
ultrawide and level of detail (Screen).

The log lists each mod at start (`mods: <name>: on, romfs, ok`) and the files it serves.

Code: `runtime/mods.c` (scan, order), `runtime/romfs.c` (player-mod overlays, `overlay_contest`), `runtime/ui/menu.cpp`
`tab_mods`. Adapted from TotK Recomp.

## Cobalt mods

Cobalt (github.com/Raytwo/Cobalt) loads Engage data mods on the Switch and in emulators, which run its ARM64 plugin.
The recompiled game cannot run it, so `runtime/cobalt.c` builds the same results at start, before the game reads
anything. A folder with `config.yaml`, `Data` or `patches` at its root is a Cobalt mod:

- `Data\...`: served like any mod's RomFS files (`.DS_Store` / `__MACOSX` junk is skipped). New bundles under
  `Data\StreamingAssets\aa\Switch` are added to the Addressables catalog (`aa/catalog.bundle`): a bundle entry, and a
  prefab entry addressed by the bundle's container path, with the bundles of its external CABs as dependencies. A
  mod's own `catalog.json` / `catalog.meta` are Cobalt's cache and are ignored.
- `patches\xml\<Name>.xml`: merged into `fe_assets_gamedata/<name>.xml.bundle` with cobalt_xml_merge's rules (tag
  diff against the game's XML; inserted rows of every mod kept, a later mod's change wins). Sheet and Book counts are
  recounted.
- `patches\msbt\message\<lang>\<region>\<file>.txt` (astra-msbt text: `[LABEL]` then the text): labels added or
  replaced in `fe_assets_message/<lang>/<region>/<file>.bytes.bundle`; `message\<file>.txt` applies to every language.

Mods apply from the bottom of the F1 > Mods list up, so the top one wins. Each merge starts from the file as the
other mods leave it (another mod's message file, for example). The result is cached in `profile\cobalt\out`
(`merged.txt` holds the key: every served mod file's size and time, the order and every patch file), so later starts
take no time; the first one lists the game's bundle CAB names once (`profile\cobalt\cabs.txt`, about 3 s).

Not supported yet: `.nro` code plugins (logged; the mod's data still loads), root-level `.lua` scripts and Cobalt's
Lua functions, `.ips` patches, new Wwise sound banks (a `.bnk` with a new name is not loaded; replaced `.wem` files
are), config.yaml dependencies (the F1 > Mods order decides), Cobalt's table expansions (AssetTable beyond the game's
capacity, more than 4096 skills).

Code: `runtime/cobalt.c` (merge, cache), `runtime/unityfs.c` (bundles, LZ4, LZMA), `runtime/mods.c` `cobalt_layout`,
`runtime/romfs.c` (`romfs_base_read`, `romfs_cobalt_overlay`, `COBALT_PRIO`).
