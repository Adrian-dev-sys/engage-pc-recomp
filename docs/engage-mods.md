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

- `patches\scripts\<name>.txt`: replaces the game's Lua script `fe_assets_scripts/<name>.txt.bundle` (no merging; the
  top mod wins).
- New Wwise banks (`Data\StreamingAssets\Audio\GeneratedSoundBanks\Switch\...\<Name>.bnk` with a name the game does
  not have) load just before the game's default banks, so their events take precedence, as in Cobalt (`audio_init`,
  hooks `cobalt_default_banks` / `cobalt_load_bank`). Replaced `.wem` / `.bnk` files are plain overlays.
- `config.yaml` `dependencies` / `optional_dependencies`: a mod is merged after the mods it depends on; a missing
  hard dependency is logged.
- Cobalt's binary patches are built in (patch set `cobalt`, `TOTK_COBALT_PATCHES`, on): larger AssetTable and skill
  tables, the DragonAttributeCommand fix.

- Voices (`runtime/cobalt_voice.c`): a character's voice event plays `<event>_<person>` when a mod's bank has it,
  `Voice="Mod!Fallback"` falls back, and `CobaltEvent:<event>_Original` markers play the game's own line.
- Root-level `<Name>.lua` scripts run when a map opens in the Somniel, with Cobalt's Lua functions (`CobaltNotice`,
  `ItemGainSilent`, `UnitSetJob`, `UnitSetLevel`, `UnitLearnJobSkill`, `ItemGift`, `MenuItemShopShow`,
  `MenuWeaponShopShow`, `MenuRefineShopShow`, `AddBondRing`, `HasPurchasedSeasonPass`). The game's Lua `Log()` writes
  to the log (`runtime/cobalt_lua.c`).
- `patches\xml\Support.xml`: additional supports per character group, with the experience curve `ExpType`
  (`runtime/cobalt_support.c`). The characters must also be in `Reliance.xml`.

- Custom sprites (`runtime/cobalt_sprite.c`), PNG files decoded by the game's own `ImageConversion.LoadImage`:
  `patches\icon\{skill,item,system}`, `emblem\{godring,godsymbol}` and `facethumb\{unit,emblem,bond}` go into the
  game's icon atlases at load; `icon\face` (bond rings), `emblem\godface` and `ui\gmap\infothumb` (world map chapter
  thumbnails) load when shown; `icon\mapstatus` adds map status icons; `icon\job\<Unit>_<Job>.png` (48x48, at most 255
  colours) replaces unit map icons; `emblem\godsymbol\<Name>.txt` (`RRGGBB` or `RRGGBBAA`) sets an emblem's colour. A
  file of the wrong size is skipped and logged. Every palette also gets a `<Palette>_<Palette>` unit icon, so a mod's
  new character has one.

Not supported yet: Cobalt's own extras (its settings menu, Phoenix mode, smithy shortcut, rumble), `MapSetSize` /
`MapSetCoords`.
Not possible: `.nro` code plugins and `.ips` code patches (logged; the mod's data still loads). They change ARM64
code, which the recompiled game does not have at run time.

Host code that holds a game object it just created keeps it on the guest stack (`roots` in `cobalt_sprite.c`) until
the game references it, since the garbage collector scans only guest stacks.

Hooks: a `hook <name> main <offset>` line in `recomp/patches-engage-v200.txt` makes that lifted function call
`runtime/hooks.c` first (regenerate with gen.py). `tools/il2cpp_map.py --find <Class::Method>` prints a managed
method's offset, parameter types and static flag (`build/engage-v200/il2cpp-methods.tsv`). `tools/il2cpp_usages.py
--find` prints the metadata-usage slot of a generic method or a class, through which a hook calls it as the game does.

Code: `runtime/cobalt.c` (merge, cache, banks), `runtime/hooks.c`, `runtime/unityfs.c` (bundles, LZ4, LZMA), `runtime/mods.c` `cobalt_layout`,
`runtime/romfs.c` (`romfs_base_read`, `romfs_cobalt_overlay`, `COBALT_PRIO`).
