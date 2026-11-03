# Go Go Ball 2 DX (E2000)

欢乐投球2.  Product string `GoGoBall2-E2000-v1-e63`; the kernel is
`2.4.31-IGS_V1.0` (27 Jul 2008).  The executable (`pitch`) is stripped;
addresses below are its own.  Players throw balls at the screen; a light
curtain of 40 × 24 infrared beams in front of it reports where they cross.

**Status:**
- Fully extracted from the stock image.
- **Plays in the loader** (machine `e2000`, plugin `gogoball2`): attract
  with sound, coins and credits, stages, score, ticket payout, game over,
  back to attract, at 60 fps on both loaders (2026-10-31).
- The protection chip is an IGS027A ("a27"), not Gold Captain's IGS036,
  with its own transport (`IGS_E2000.md`, IGS027A).  Its ROM is not
  dumped; the attract order and the game flow are reconstructed (see
  "Loader status").

## Playing

```powershell
.\play.ps1 gogoball2_dx        # WSL
.\play-win.ps1 gogoball2_dx    # native Windows
```

The attract starts after the light-curtain check (about 15 s).  A credit
(4 coins by default) starts the game at once.

| Key | Cabinet |
| --- | --- |
| 5 / 6 | coin 1 / coin 2 |
| Arrows | move the crosshair |
| Space, Ctrl | throw a ball at the crosshair |
| 1, Z | KEY1 (start, select) |
| 2, X | KEY2 (enter) |
| F2 | test |
| 9 | service |
| F1 | show / hide the key help |

- **The game's own keys:** Esc quits it, `/` starts its warm-up clock.
- **Settings and bookkeeping** (the IGS027A's NVRAM) are kept in
  `/rf_data/igs027.nv` of the rom set, next to the game's own files (for a
  `.7z` set that is the cache, `<cache>/linux/gogoball2_dx/rf_data/`).
  Delete it for factory settings.
- **A throw** blocks the light-curtain cell under the crosshair for 3
  frames.  `GGB_AIM=<n>` sets the crosshair speed in pixels per frame
  (default 8).
- **`GGB_REGION=<n>`** is the region the IGS027A reports (default 4): 1
  CN, 2 HK, 3 FA, 4 US, 5 KR, 6 TL, other TW.  It picks the version text
  and the font set.
- **`GGB_SCRIPT="sec:key[+],..."`** presses keys on a timetable for
  testing.  Keys are `coin coin2 key1 key2 test service left right up down
  throw rthrow` (`rthrow` throws at a random point); a press lasts 0.2 s, or
  with `+` until the next entry.

## Extracting

`igs-mkromset gogoball2_dx <image>` builds the rom set;
`romtool/igs_e2000_extract.py gogoball2_dx <image> <work dir>` a full
workspace.

| Path | What |
| --- | --- |
| p1 ext2 | LILO map and kernel `GoGoBall2-OS-e63` |
| p2 | root filesystem (plaintext ROFS); `/etc/rc.d/rc0.d`..`rc7.d` the loader and executable pieces |
| p3 | IGS_SHFS game data at `/GoGoBall2`: `main/*.rom`, `sound/`, `system/`; `linux/pitch` is a decoy |
| p4 ext3 | `/rf_data`: the game's settings, logs and ranking |

## How the cabinet starts it

1. The kernel mounts p3 on `/GoGoBall2` and p4 on `/rf_data`.
2. `/bin/csh` and `/mnt/GECA` mount a ramfs on `/GoGoBall2/linux` and
   build `pitch` there (`IGS_ROFS_REVERSE_ENGINEERING.md`, Executable
   assembly).  The static `pitch` underneath, and `/pitch/exec/pitch` on
   p2, wipe the disks if run.
3. `/etc/X11/IGS` starts XFree86 and runs `cd /GoGoBall2; ./pitch` in a loop.

## The executable

Linked like Gold Captain's: `libGL`, `libX11`, `libXext`, `libpthread`,
`libdl`, `/lib/libcasmcard.so`, with SDL 1.2 (X11/GLX, OSS `/dev/dsp`) and
SDL_mixer built in.  It also carries libng (v4l) for a camera that is
optional (`c/ccd.c`; no `/dev/video0`, no camera), a member-card reader
(`app/c/creader.c`, Cas* and SLE4442 calls) and a ranking client
(sockets).  Devices: `/dev/plx/Pci9030-0`, `/dev/roio` (Gold Captain's
accepted BIOS), `/dev/pccard0` (ioctl `0xC8` every frame, ignored).

## Loader status

Done:
- [x] Rom set: `root p2; mount p3 /GoGoBall2; assemble
      /GoGoBall2/linux/pitch; mount p4 /rf_data`.
- [x] `machines/e2000`: the IGS027A transport.
- [x] `games/gogoball2`: sprite and object stores, settings blocks, frame
      packet (keys, light curtain, coins), ticket dispenser, ball-stopper
      motor, lamps, password, re-keying, version.
- [x] `libcasmcard` stand-in: no card service, so no member card.

Reconstructed IGS027A logic:
- [x] **Attract order** (type 17, functions 0/1): each scene in turn.
- [x] **Game flow** (type 17, functions 2/3): how to play → stage select
      → stage → stage over → next stage, or score → tickets → game over.
      A game that clears no stage goes straight to game over (the score
      screen divides by the stages cleared).
- [x] **Light curtain**: one cell per throw, mapped from the calibration
      as the game maps it back (`sub_8065340`).

Open:
- [ ] The real attract order and flow table, the region, the default
      password (would need the IGS027A ROM).
- [ ] Not seen: the operator menus beyond the sensor setting, the member
      card, the camera, the ranking server.
- [ ] On Linux the game's `/rf_data/...` writes point at the host and fail;
      harmless.
