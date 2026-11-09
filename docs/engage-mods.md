# Mods (Engage)

Put each mod in its own folder under `mods\` next to `engage.exe` (`TOTK_MODS_DIR` overrides). The layouts Ryujinx,
Yuzu and Atmosphere use all work:

- `mods\<name>\romfs\...`
- `mods\<name>\atmosphere\contents\0100A6301214E000\romfs\...` (any case of the title id)
- `mods\<name>\contents\0100A6301214E000\romfs\...`, `mods\<name>\0100A6301214E000\romfs\...`

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
