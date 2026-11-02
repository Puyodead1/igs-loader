# Percussion Master 3 (E2200)

Percussion Master 3, model `M107FA` (on the operator menu
`M: M107FA`, `G: V107.0`), built December 2012.  A two-player drum game: each
player has a drum (a surface split into a left and a right half, with a
left and a right rim) and two little drums, blue "1" and red "2"; notes run
down a blue, a centre and a red lane.  A 12-key pad per player enters
member accounts.  Online: member accounts, rankings, contest, holiday and
"dragon" boards, a patch server.

**Status:**
- Fully extracted from the stock image: an encrypted root filesystem (a
  new `igs_rofs` profile), SquashFS images for the data, the launcher and
  the executable FPGA-cipher encrypted (`IGS_E2200.md`).
- **Plays in the loader** (machine `e2200`, plugin `percussionmaster3`),
  both loaders: camera error, IGS logo, coin page with its videos and
  sound, the demo song, coins, guest login, select type, mode, level and
  song, songs with the drums judged, combo and score, the round result,
  the next rounds, the final result, ranking, thanks, back to the logo; the
  operator menu on F2 (2026-11-02).
- The IGS036 runs the screen flow and holds the chart keys; its firmware
  is not dumped.  The plugin rebuilds the flow from what each screen
  reports (`IGS_E2200.md`); the game logic, judging and scores are the
  executable's own.
- **Offline:** no IP network (list `env` `IGS_NO_NET`): guest play, the
  "OFFLINE" mark.  Nothing of the server is emulated.

## Playing

```powershell
.\play.ps1 percussionmaster3_m107fa        # WSL
.\play-win.ps1 percussionmaster3_m107fa    # native Windows
```

The camera error (no camera) shows for about 20 s, then the logo and the
coin page, about 50 s after start on Windows.  A coin and the drum surface
start a game; the dumped settings ask 1 coin a credit.

| Key | Cabinet |
| --- | --- |
| A, F | player 1 little drums 1 (blue), 2 (red) |
| S, D | player 1 drum surface, left and right half |
| W, E | player 1 drum rim, left and right |
| Space | player 1 surface, both halves |
| J, ; | player 2 little drums 1, 2 |
| K, L | player 2 drum surface halves |
| I, O | player 2 drum rims |
| Enter | player 2 surface, both halves |
| Numeric keypad 0..9, *, / | player 1 number keys 0..9, *, # |
| Shift + numeric keypad | player 2 number keys |
| 5 | coin |
| 9 | service (a credit) |
| F2 | test (operator menu) |
| F1 | show / hide the key help |

- **Drums:** the drum keys are the game's own developer keyboard.  Big
  notes want both halves (or both rims) at once: Space / Enter, or two
  keys together.
- **Menus:** the little drums move, the surface confirms; the operator
  menu the same with player 1's drums ("1P Drum surface: Confirm").  Leave
  the I/O test by hitting both players' surfaces together.
- **Hit strength:** the drums' sensors report a level; the game shows it
  only in the operator menu's sensor test, and a hit counts when it reaches
  the sensor's threshold (40 by default, set in that test).
  `PM3_VELOCITY=n` (1..255, default 160) is the level a key press reports.
- **Screen:** the cabinet runs X at 1280x720 (`etc/X11/xorg.conf`) and
  the game asks for a fullscreen window that size; Ogre takes the screen's
  size for it while the menus keep their 1280x720 layout.  The list's `env`
  `IGS_SCREEN=1280x720` gives it the cabinet's screen, so it runs in a
  1280x720 window; on Windows `IGS_FULLSCREEN=1` scales that frame to the
  monitor.
- **Settings:** the game's own files in `/PM3/rewrite` (the cabinet's
  `SettingInfo.dat`, bookkeeping, rankings, top scores), in the rom set (for
  a `.7z` set, the cache's
  `<cache>/<linux|windows>/percussionmaster3_m107fa/PM3/rewrite/`).  The
  sensor thresholds are the IGS036's and are not kept between runs.

## Extracting

`romtool/mkromset.py percussionmaster3_m107fa <image>` builds the rom set
(`IGS_LOADER.md` has its size).

| Path | What |
| --- | --- |
| p1 ext3 | LILO map `PM3`, kernel `PM3-OS` (2.6.32.8, `E2200 BIOS V1.0.0 (20111207)`) |
| p2 | root filesystem, `igs_rofs` (profile `pmaster3`, key literal `E2200-PMaster-V3`): Fedora-based, X.org, NVIDIA libGL, SDL, SDL_mixer; `/PM3/PowerOn` (encrypted launcher), `/bin/uudecode` (its decrypter), `/bin/mountloop` |
| p3 ext3 | `/PM3`: `ROMS/*.rom` (32 SquashFS 4 images), empty mount points `exe`, `data`, `media` |
| p4 ext3 | `/PM3/rewrite`: settings, bookkeeping, rankings, photos, the network and store configuration, downloads |

The set has the images' contents at their mount points (`exe.rom` at
`/PM3/exe`, `data.rom` at `/PM3/data`, the rest at `/PM3/media/<name>`);
the images themselves are left out.  `/PM3/exe/pm3` stays as dumped; the
decrypted executable is `/PM3.bin/pm3`, as on the cabinet.

## How the cabinet starts it

1. The kernel mounts p2 as root, mounts a ramfs on `/PM3.bin` and runs
   `/bin/uudecode /PM3/PowerOn /PM3.bin/PowerOn Hello-Percussion`: the
   FPGA decrypts the launcher with that key string.
2. `/etc/X11/IGS` mounts p3 on `/PM3` and p4 on `/PM3/rewrite`, starts
   `dhclient` and X.org, then in `/PM3/exe` runs `/PM3.bin/PowerOn` once,
   `./pcli` (a network client) and `/PM3.bin/pm3` in a loop.
3. `PowerOn` applies downloaded updates (`rewrite/update`, versions from
   `machine.cfg` / `poweron.cfg`), runs `/bin/mountloop` (the SquashFS
   images onto `exe`, `data` and `media/*`) and decrypts `../exe/pm3` to
   `/PM3.bin/pm3` (a key-id header and the FPGA cipher, `IGS_E2200.md`).

The loaders run `/PM3.bin/pm3` alone, in `/PM3/exe`.

## The executable

`pm3` (i386, 26 MB, dynamic symbols kept) is an Ogre 1.6.1 game with
Scaleform GFx menus, Lua, OIS, Theora videos (`.ogv`), Ogg Vorbis music
through SDL_mixer and a V4L camera (`/dev/video0..9`, "CCD").
- **IGS036** (`Igs036P2A`, `Game::iIgs036_*`): Hero of Robots' loader and
  request/response transport, its own framing and messages
  (`IGS_E2200.md`).
- **Screens** (`PMGAME::*`, `GameProcess`): 23 state objects, sent to the
  chip as a table; the chip names the current one each frame.
- **Charts** (`PMGAME::BandScript`): text scripts (`TRACK`, `EVENT`,
  `ChangeBPM`, ...) in `media/script_AES/*.enccut`, 814 charts (166 songs,
  Easy, Normal, Hard, Special, Master), encrypted, their first 16 bytes
  held by the chip; `SongList.enc` lists them.  The dump also has two
  plaintext leftovers in `rewrite/script_download` (`SongList.txt`,
  `p001_easy.txt`), which the game ignores.
- **Judging** (`StagePy::UpdateInput`): per player four lines: the
  surface, the rims, little drums 1 and 2; the best grade within 7 frames
  (1/60 s), the next within 12.5, the last within 14.
- **Network** (`NetManager`, `pcli`): the game server `210.5.22.170:9999`
  and patch server (`rewrite/netGame.cfg`, `netPatch.cfg`), FTP
  `210.5.22.171:6001`; the store id is kept FPGA-encrypted in the
  IGS036 (messages 14, 15, 16).  Offline it plays guests only.  Protocol,
  commands and a stand-in server plan: `PercussionMaster3_Online.md`.
- **Shell commands:** `cp` of downloaded lists, `rm` of logs and records
  (operator reset), `hwclock`, `./checkUpdate`.  On Linux they run in the
  set; on Windows they are skipped.

## Loader status

Done:
- [x] Rom set: `root p2; mount p3 /PM3; loop /PM3/ROMS/exe.rom /PM3/exe;
      loop /PM3/ROMS/data.rom /PM3/data; loop /PM3/ROMS/*.rom
      /PM3/media/*; mount p4 /PM3/rewrite; poweron /PM3/exe/pm3
      /PM3.bin/pm3`.
- [x] `machines/e2200`: machines/e2100 under its own name; the asset
      cipher takes its mask from the plugin by key.
- [x] `games/percussionmaster3`: channel and framing, the state table and
      flow, the I/O board (keys, coins, twelve drum sensors with levels and
      thresholds), chart keys and cut bytes, version, coin record.
- [x] Core: older GLEW never matches the last name of the extension
      string; `IGS_CG_ARB` now ends it with a space (Ogre 1.6's fp40).
      `IGS_SCREEN` (the X screen's size, no RandR); window-manager
      fullscreen requests dropped unless `IGS_FULLSCREEN`.  Windows: with
      `IGS_FULLSCREEN` an X11 fullscreen window covers the monitor and the
      frame is scaled to it.

Invented:
- [x] The screen flow (from the results each screen reports: the
      routing in the plugin's `next_state`), the attract order (coin page,
      demo song, ranking, logo).
- [x] IGS036 version 100, fuses good, FPGA version 1.0; sensor thresholds
      255 / 1 / 40 / 40 (max, min, default, current); an empty coin record.
- [x] The chart keys: PowerOn's key table (key 5 checked); the cut
      bytes rebuilt from every chart's first line (`IGS_E2200.md`).

Open:
- [ ] Not seen: member accounts and login, special and battle modes'
      own screens (event, skill, role select, match), the dragon, holiday
      and contest boards, photos (no camera), continue.
- [ ] The camera error at start-up (about 20 s): there is no camera.
- [ ] Lamps and meters (message 3 out) are ignored.
- [ ] Chip messages 14 / 16 (the network registration) are not answered,
      so the game never connects.
- [ ] A local game server (`PercussionMaster3_Online.md`).
- [ ] Linux: the instancing shaders (`vp40`) fail to compile as `arbvp1`;
      the scenes still draw.
