# Speed Rider (E2000)

极速摩托.  Product string `SR-E2000-v1-e64`, model `M-109CNC`; the
executable (`SR`) is stripped, Ogre 1.4.9 with OIS, Cg and FreeImage;
addresses below are its own.  A motorbike racer: throttle grip, brake
lever, the bike leans to steer; SLE4442 member card; up to four cabinets
linked over RakNet.

**Status:**
- Fully extracted from the stock image (`cut` assembly mode).
- **Plays in the loader** (machine `e2000`, plugin `speedrider`): attract
  with sound, coins and credits, mode select, story and time-attack races
  with AI riders, continue, game over, ranking, back to attract, on both
  loaders (2026-10-31).
- The IGS036 talks a message transport of its own (`IGS_E2000.md`, IGS036
  message transport).  Its ROM is not dumped; the UI table is
  reconstructed, the game-mode table and the AI riders' throttle law are
  invented (see "Loader status").

## Playing

```powershell
.\play.ps1 speedrider_m109cnc        # WSL
.\play-win.ps1 speedrider_m109cnc    # native Windows
```

Loading takes about a minute on Windows and several on WSL (1.4 GB of zip
archives).  A credit (2 coins by default) and S/Boost start a game.

| Key | Cabinet |
| --- | --- |
| 5 | coin |
| Up | throttle |
| Down | brake |
| Left / Right | lean |
| 1, Enter, Space | S/Boost (start, confirm, boost) |
| W / S / A / D | panel up, view/down, left, right |
| X | sensor |
| F2 | test |
| 9 | service |
| F1 | show / hide the key help |

- Throttle, brake and lean ramp like the real controls; `SR_LEAN=<n>` sets
  the frames to full lean (default 15).
- **Single cabinet:** link play (多人连线) is refused; mode select comes
  back.  No member card: the game plays card-less ("您是无卡玩家") and
  shows a card-reader warning on the title.
- **Settings and bookkeeping** (the IGS036's NVRAM) are kept in
  `/rewrite/igs036.nv` of the rom set (for a `.7z` set, the cache's
  `<cache>/<linux|windows>/speedrider_m109cnc/rewrite/`).  Delete it for
  factory settings.  The game's own records (ranking, ghosts, logs) are in
  the same folder.
- `SR_TRACE=1` logs every sub-message and button event; `SR_PREFIX_DIR=<dir>`
  writes each rebuilt zip prefix there.

## Extracting

`igs-mkromset speedrider_m109cnc <image>` builds the rom set;
`romtool/igs_e2000_extract.py speedrider_m109cnc <image> <work dir>` a full
workspace.

| Path | What |
| --- | --- |
| p1 ext2 | LILO map, kernel; GECA stub at sector 7145 |
| p2 | root filesystem (plaintext ROFS): `/etc/rc.d/rc0.d`..`rc9.d` the loader and executable pieces, `/usr/lib/libstdc++.so.5` |
| p3 | IGS_SHFS game data at `/sr/Game`: `bin_linux/` (Ogre, OIS, Cg, plugins), `data/` (scripts, sounds, `zip/*_2.zipsep`) |
| p4 ext3 | `/rewrite`: ranking, ghosts, logs |

## How the cabinet starts it

1. The kernel mounts p3 on `/sr/Game` and p4 on `/rewrite`.
2. `/bin/csh` and `/mnt/GECA` mount a ramfs on `/sr/exec` and build `SR`
   there (`IGS_ROFS_REVERSE_ENGINEERING.md`, Executable assembly, `cut`).
3. `/etc/X11/IGS` starts XFree86 and runs `cd /sr/Game/bin_linux; ./SR`
   (a link to `../../exec/SR`) in a loop.

## The executable

Links `libOgreMain-1.4.9.so`, `libOIS-1.0.0.so`, `libfreeimage.so.3`,
`libcasmcard.so` (from `bin_linux`, its RPATH `./`), `libstdc++.so.5`,
`libbz2`.  Ogre loads its GL render system, scene managers, Cg program
manager and `libZipSepArchive` from `bin_linux`.  OpenAL (Loki, OSS
`/dev/dsp`) is built in.  Devices: `/dev/plx/Pci9030-0`, `/dev/pccard0`
(ioctl `0xFE` once).

**Zip archives.**  `data/zip/<name>_2.zipsep` are the stored zip archives
minus their first 2000 bytes.  `libZipSepArchive` asks the game for the
missing part (an Ogre plugin object whose callback is `sub_82DA320`), which
asks the IGS036 (sub-message 2).  The plugin rebuilds the prefixes:
- local headers from the central directory (all files are stored);
- data of files another archive holds intact (ShangHai(A) `ag00402.dds` =
  ogre `c_ag00402.dds`, TaipeiA `ag03.dds` = Taipei `ag03.dds`);
- Taipei `ag03-02.mesh` (86 bytes lost) exactly, from the layout of its
  siblings (CRC-checked);
- stand-ins for what no other file holds: `2nd_floor.dds` (ogre),
  `ag00501_da.dds` (HeyLong(A)): the usual header for their size, black
  rows; `ag00401-01.mesh` (GreatDam(A)), `ag03-01.mesh` (Taipei): the
  sibling meshes' layout with the vertices collapsed, so the part draws
  nothing.

**Shaders.**  The Cg programs ask only for NVIDIA's `vp40` / `fp40`
profiles.  On Mesa the core's `IGS_CG_ARB=1` (the list's `env`) claims
those extensions and has Cg compile them as `arbvp1` / `arbfp1`.

## Loader status

Done:
- [x] Rom set: `root p2; mount p3 /sr/Game; assemble /sr/exec/SR; mount p4
      /rewrite`.
- [x] `machines/e2000`: the IGS036 message transport.
- [x] `games/speedrider`: inputs as header words and button events, coins,
      versions, NVRAM, zip prefixes, UI table, game modes, AI riders,
      member-card tables.
- [x] `libcasmcard-nocard` (the E2000 stub under SONAME `libcasmcard.so`):
      no card service.

Reconstructed or invented IGS036 logic:
- [x] **UI** (`0xC1` → `0x41`): reconstructed from the game's flagged
      show/hide calls (`sub_834E030`).
- [x] **Game modes** (`0x80`.. → `0x20`): invented order, link play left
      out.
- [x] **AI riders** (`0xC0` → `0x40`): throttle 0.4 + 0.1 × speed error,
      brake past −2; invented.
- [x] Member-card bit order (identity) and key; the stream's key index (0).

Open:
- [ ] The real mode table, AI law and member-card tables (would need the
      IGS036 ROM).
- [ ] Not seen: link play, the member card, the test and operator menus
      beyond entering them, the lamps (`0x20`..`0x27`).
- [ ] On Linux the game's `/rewrite/...` writes point at the host and fail;
      harmless.  Races run at about 7 fps on llvmpipe.
- [ ] On Windows the vp40/fp40 shaders need an NVIDIA GL driver (tested);
      `IGS_CG_ARB` is Linux-only.
