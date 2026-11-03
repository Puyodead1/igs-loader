# Speed Driver 2 (E2000)

极速 Speed Driver 2 Over Take.  Product string `SD2-E2000-v05-e61`; the
executable (`sd2`, 2008) is stripped; addresses below are its own.  A
driving game: wheel, gas and brake, gear up/down, View, N2O, Start; SLE4442
member card (car refits, story progress); up to four cabinets linked.

**Status:**
- Fully extracted from the stock image (`cut` assembly mode).
- **Plays in the loader** (machine `e2000`, plugin `speeddriver2`), both
  loaders: attract (the in-engine live show) with music, coins and credits,
  mode, car, gearbox, course and direction select, the race with rivals,
  the game's result, ranking with name entry, continue with a credit, game
  over, back to the attract; the operator menu on F2.
- The IGS036 directs the game (`IGS_E2000.md`, Speed Driver 2 layout): main
  state, texture and sound pages, the select screens' 2D sprites, game data,
  buttons and race commands, ranking, the AI throttle.  Its ROM is not
  dumped: the screens are rebuilt from the game's art and data, their layout
  and timing are invented.

## Playing

```powershell
.\play.ps1 speeddriver2        # WSL
.\play-win.ps1 speeddriver2    # native Windows
```

The live show starts after about 10 s.  A coin and Start open the mode
select.

| Key | Cabinet |
| --- | --- |
| 5 | coin |
| 1, Enter | Start |
| Up / Down | gas / brake |
| Left / Right | wheel; in the menus, the cursor |
| Q / W | gear up / down (MT; operator menu: down / up) |
| X | N2O |
| C | View |
| F2 | operator menu (in the attract); again: its next page, after the last back |
| F1 | show / hide the key help |

- Wheel, gas and brake ramp like the real controls; `SD2_STEER=<n>` sets the
  frames to full lock (default 20).
- Start works once the live show is running (after its loading screen).
- **Modes:** time attack (course, direction; ranking per course), story
  (stages in order; the next after a win), overtake (course).  20 cars, AT
  or MT.  Each select screen times out after 20 s.
- **Ranking:** after a time-attack race the game says whether the time ranks
  (0x30); then the name entry (Left / Right pick a letter or END, Start takes
  it, 30 s), then the course's table.  `SD2_TEST_RANKIN=1` forces the name
  entry (testing).
- **Continue:** 10 s; needs the continue coins (operator settings).
- **Single cabinet, no member card:** the game finds no network (`ifconfig`
  is not run) and no card reader.
- **Settings and records** are the game's own files in `/data` of the rom
  set (for a `.7z` set, the cache's `<cache>/<linux|windows>/speeddriver2/data/`):
  `storesetup.bin` (coins), `gamesetup.bin`, `rank.bin`, replays.
- `SD2_SCRIPT="sec:key[+],..."` presses keys on a timetable, or with
  `@screen+sec:key` that long after a chip screen opened (keys `coin start
  left right gas brake n2o view up down test`; screens 0 mode, 1 car, 2
  gearbox, 3 course, 4 direction, 5 name, 6 ranking, 7 continue, 8 game
  over); `SD2_AUTO=1` lets the game's autopilot drive (testing).
## Extracting

`igs-mkromset speeddriver2 <image>` builds the rom set;
`romtool/igs_e2000_extract.py speeddriver2 <image> <work dir>` a full
workspace.

| Path | What |
| --- | --- |
| p1 ext2 | LILO map, kernel; the executable's first KiB at sector 7145 |
| p2 | root filesystem (plaintext ROFS): `/bin/HWtest`, `/etc/rc.d/rc4.d` (GECA), `/lib/libc<d>.so` (pieces), `/usr/lib/libstdc++.so.5` |
| p3 | IGS_SHFS (plain layout) at `/race/linux`: `data/` (`data.ZPK` 238 MB, `data.hdr`, `inf/`, `sound/`, `replay/`), `io.inf` |
| p4 ext3 | `/data`: settings, bookkeeping, ranking, replays |

## How the cabinet starts it

1. The kernel mounts p3 on `/race/linux` and p4 on `/data`.
2. `/bin/HWtest` and `/mnt/GECA` build `/race/exec/sd2` on a ramfs
   (`IGS_ROFS_REVERSE_ENGINEERING.md`, `cut`).
3. `/etc/X11/IGS` starts XFree86 and runs `cd /race/linux; ./sd2` in a loop.

## The executable

Links SDL 1.2, GL, GLU, zlib, ncurses 5, libstdc++ 5 and `/lib/libcasmcard.so`
by path.  Loki OpenAL is built in (OSS `/dev/dsp`, opened by the chip's
first sound page).  It allocates and touches 350 MB at start.
- **`data.ZPK`** holds the assets; `data.hdr` its directory, missing its
  first 128 bytes, which the chip sends (0xB2).  The plugin's copy is rebuilt
  from the file records and `2dtexture.inf`'s paths; its XOR matches the
  file's check byte.
- **Shell commands:** `aumix` (volume), `ifconfig`, `route`,
  `/race/data/option.s` (PPPoE), `hwclock`.  The loaders do not run them
  (list `env` `IGS_NO_SYSTEM`), so `popen("ifconfig eth0")` fails and the
  game runs without network.
- **Absolute paths** (`/data/...`) resolve inside the set (list `env`
  `IGS_SET_DIRS=/data` on Linux).
- Debug keyboard: the game polls SDL keys with Shift/Ctrl combinations
  (physics tuning, camera, autopilot); Space, `d`, F7..F12 and the keypad
  also act without a modifier.  The plugin's keys avoid them.

## Loader status

Done:
- [x] Rom set: `root p2; mount p3 /race/linux; assemble /race/exec/sd2;
      mount p4 /data`.
- [x] `machines/e2000`: the IGS036 message transport, Speed Driver 2 layout.
- [x] `games/speeddriver2`: header inputs, buttons and race commands
      (gears, view, N2O), coins, credits, the `data.hdr` prefix, 2D and
      sound pages, title music, the select screens, game data, the AI
      throttle, ranking and name entry, continue, game over, the operator
      menu's pages.
- [x] The race's 2D pages stack on page 1, as its HUD's image indices
      expect (else the timer and speed digits show the 警告 banner and the
      sparkle images).
- [x] Core: `IGS_SET_DIRS`, `IGS_NO_SYSTEM`, `IGS_NO_AUDIO`.

Invented:
- [x] The screens' layout, backdrops, timing and order, the credits
      overlay, the name entry's letter wheel, F2 stepping through the
      operator pages, header words +28 / +68 (frame counters).
- [x] The AI law is the game's own overtake-mode law with the controllers'
      default thresholds, not the chip's.

Open:
- [ ] The refit, card and bonus screens (0x85/0x86, 0x9C..0x9F: member card
      data), link play, the attract's ranking pages, a 3D car showcase in the
      car select (state 5).
- [ ] The operator menu: how the real chip moves between its pages (here
      F2); its settings pages beyond those seen.
- [ ] Story stages after the first two and overtake were run only briefly.