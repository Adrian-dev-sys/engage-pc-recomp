# PC quality of life (Engage)

- **Keyboard**: Engage has its own defaults (`runtime/input_map.c` `ENGAGE_KEYS`): WASD cursor, arrows D-pad, IJKL
  camera, Space/Enter A, Shift/Backspace B, F X, R Y, Q/E L/R, Z/C ZL/ZR, Tab +, M -. Rebinding in F1 > Controls.
- **Mouse**: the mouse camera is off by default on Engage (`cfg.c` `ENGAGE` defaults: `MOUSE_SENS=0`), so clicks no
  longer capture the cursor.
- **Screenshots**: F12 saves the presented frame, without the menu or overlay, as a PNG in `screenshots\` next to the
  exe (`runtime/screenshot.c`; encoded on a worker thread). For tests, `TOTK_SHOT_AT_FRAME=1500,3000` (environment)
  also saves one at those presented frames.
- **Fast-forward**: T, or L3+R3 on a gamepad, toggles it; F1 > Game > "Fast-forward speed" (2-4x, `fast_forward`).
  The guest clock (`main.c` `rt_speed_set`) and the per-frame game step (`frame_rate.c` `engage_frame`) are both
  multiplied, so Unity time and frame-counted logic stay together. Music plays at normal speed. On Engage, L3+R3 no
  longer steps the performance overlay (F10 still does).
- **Mute in background**: F1 > Sound (`mute_background`), useful with "Keep running out of focus".
- **Save backups**: besides the three commit backups in `profile\save-backup`, each start copies the save to
  `profile\save-session\<UTC time>` (newest 5 kept; `fs_host.c` `session_backup`).
- **Play time**: F1 > Game, kept in `profile\playtime.txt` (`runtime/playtime.c`).
- **Amiibo**: F1 > Amiibo lists the dumps (540-byte `.bin` / `.nfc`) in `amiibo\` next to the exe (`amiibo_dir`
  overrides). Start the scan at the Somniel's amiibo gazebo, then pick one; it is lifted after 2.5 s unless "Lift it by
  itself" is off. `runtime/amiibo.c` (reader state), `ov_system.c` (nn::nfp), `ui/amiibo_tab.cpp`.
- **Fake online** (experimental, off): F1 > Game (`fake_online`). Chapter maps get the online bonus spots (Kill /
  Killed tiles, `MapKillBonus`) from random local counts on walkable play-area cells instead of the server.
  `runtime/engage_online.c` hooks `MapSequenceNet.Download` (binds the download proc without the game's online byte,
  so rankings, profiles and the clear-time upload stay offline) and `NexKillBonus.Download` (fills the grids, reports
  success). Suspended maps keep their spots; a new chapter start rolls new ones. The chapter rankings (world map
  menu > Ranking, last-chapter hub menu) open without the connect prompt (the menu items' `ACall` runs with the online
  byte set, restored after) and `NexRanking.Download` returns 100 unit + Emblem pairs from the units and Emblems the
  player has, seeded by the chapter id.

amiibo.c and playtime.c are adapted from TotK Recomp (MIT).
