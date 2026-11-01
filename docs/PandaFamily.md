# Panda Family (E2000)

熊猫家族.  Product string `PF-E2000-v1-e65`, model `M100CN`.  The executable
(`PF`) is stripped; addresses below are its own.  Two players, four buttons
each (yellow, red, red, yellow), a ticket dispenser per player and an
optional USB camera that photographs the player for the ranking.  Mini
games: climb the bamboo, fire fighter, buy sweets and others.

**Status:**
- Fully extracted from the stock image.
- **Plays in the loader** (machine `e2000`, plugin `pandafamily`): attract
  with sound, coins and credits, stage select, stages, ranking, ticket
  payout, back to attract, at 60 fps on both loaders (2026-11-01).
- The game logic is all in the executable.  The IGS036 is the I/O board,
  the settings store and the action-group allocator, behind a transport of
  its own (`IGS_E2000.md`, IGS036 packet transport).
- No camera: the game skips the photo and shows its stock faces.

## Playing

```powershell
.\play.ps1 pandafamily_cn        # WSL
.\play-win.ps1 pandafamily_cn    # native Windows
```

The attract starts after about 15 s.  A credit (2 coins by default) and a
button start a game.

| Key | Cabinet |
| --- | --- |
| 5 | coin 1P |
| 2 | coin 2P |
| A S D F, or Left Z X Right | 1P buttons: yellow, red, red, yellow |
| H J K L | 2P buttons |
| F2, 6 | test |
| 9, V | service |
| F1 | show / hide the key help |

- **The game's own keys** (its keyboard fallback, live in this build): `a s
  d f h j k l v 6 7 8` are the twelve key bits, `t` pays a ticket by hand,
  Esc quits.
- **Test menu:** yellow buttons move, red buttons confirm.
- **Settings and bookkeeping** (the IGS036's NVRAM) are kept in
  `/PF/pf_data/igs036.nv` of the rom set, next to the game's ranking (for a
  `.7z` set, the cache's `<cache>/<linux|windows>/pandafamily_cn/PF/pf_data/`).
  Delete it for factory settings.
- **`PF_SCRIPT="sec:key[+],..."`** presses keys on a timetable for testing.
  Keys are `coin coin2 a s d f h j k l test service`; a press lasts 0.2 s,
  or with `+` until the next entry.

## Extracting

`romtool/mkromset.py pandafamily_cn <image>` builds the rom set;
`romtool/igs_e2000_extract.py pandafamily_cn <image> <work dir>` a full
workspace.

| Path | What |
| --- | --- |
| p1 ext2 | LILO map, kernel; the executable's first KiB at sector 6658 |
| p2 | root filesystem (plaintext ROFS): `/bin/csh`, `/bin/tcsh` + `/bin/ksh` (GECA), `/lib/libfun1..6.so` (the pieces), `/lib/libIGS_API.so`, SDL in `/usr/X11R6/lib`; `/linux/PF` a decoy |
| p3 | IGS_SHFS game data at `/PF`: `main/*.rom`, `sound/` (plain WAV), `flc/`, `preview/`, `demoplay/`; `PF` links to `/linux/PF` |
| p4 ext3 | `/PF/pf_data`: ranking, `ccd/`, `pics/` (stock photos), `volume.desc` |

## How the cabinet starts it

1. The kernel mounts p3 on `/PF` and p4 on `/PF/pf_data`.
2. `/bin/csh` writes `/mnt/GECA` (the first KiB of `/bin/tcsh` + `/bin/ksh`)
   and the head, and GECA builds `/linux/PF` on a ramfs from the pieces
   (`IGS_ROFS_REVERSE_ENGINEERING.md`, Executable assembly).  The static
   `/linux/PF` on p2, under the ramfs, is a decoy.
3. `/etc/X11/IGS` starts XFree86 and runs `cd /PF; ./PF` in a loop
   (`LD_LIBRARY_PATH=/usr/X11R6/lib`).

## The executable

Unlike the other E2000 games it links SDL 1.2 and SDL_mixer dynamically,
with `libGL`, `libGLU`, `libz`, `libstdc++.so.5` and `libIGS_API.so`.
- **`libIGS_API.so`** is the PLX SDK 4.3 (`Plx*`) plus RC4, AES, MD5, the
  `/dev/roio` BIOS check, `/dev/pccard0` calls (`PCCARD_*`, `TTY_*`) and
  `CameraInfo_*`.  The game imports only `PlxPciDeviceFind`, `Open`,
  `BarMap`, `BoardReset` and `CameraInfo_*`; its transport, RC4 and OpenSSL
  copy are its own.  The library runs unchanged on the machine's PLX
  devices, so it is not replaced.
- **Camera** (`sys/c/ccd.c`, xawtv's libng): `CameraInfo_Read` lists
  `/proc/video/dev/video0..4` and accepts a TP6800, TP6810, OV511 or IP293
  USB camera.  Neither host has that file, so the camera type is 0, and the
  photo (`sub_808C2D0`: camera found and the Camera setting on) is skipped.
  `/dev/video*` is never opened.
- **libvlc and libhal** are not used: their names are only in the decoy
  bytes at the start of the piece files (`libfun2.so`, `libfun3.so`).
- `system("free")` between games; `hwclock -w` from the clock setting.

## Loader status

Done:
- [x] Rom set: `root p2; mount p3 /PF; assemble /linux/PF; mount p4
      /PF/pf_data`.
- [x] `machines/e2000`: the IGS036 packet transport; the board reset
      restarts the chip.
- [x] `games/pandafamily`: keys, coins, tickets, settings, action groups.
- [x] Windows: the set's `/usr/X11R6/lib` is searched (SDL_mixer, GLU,
      Vorbis).

Invented:
- [x] I/O board versions (`I:S100CN`, `O:S100CN`), fuses OK, the I/O
      register dump (0x15) and the photo booth's key block (0x17), all 0.

Open:
- [ ] Not seen: the camera and photo booth, 2P play, ticket and fuse
      faults, the operator menus beyond the main menu.
- [ ] Key bits 10 and 11 (the game's `7`, `8`): use unknown.
