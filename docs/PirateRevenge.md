# Pirate Revenge (E2000)

Product string `PirateRevenge-E2000-v05-e61`; the executable (`sship`) was
built 4 Aug 2008 and keeps its symbols; the kernel is `2.4.31-IGS_V0.5a`
(2 Aug 2007).  Same engine as Gold Captain (`GoldCaptain.md`).

**Status:**
- Fully extracted from the stock image.
- **Plays in the loader** (machine `e2000`, plugin `piraterevenge`):
  attract with sound, coins and credits, ship select, stage, boss, continue,
  game over and name entry, then back to attract, at 60 fps on both loaders
  (2026-10-31).
- The IGS036's own game logic (game flow, weapon table, WarZones) is in its
  internal ROM, which is not dumped.  The plugin reconstructs it from the
  executable (see "Loader status").  The weapon values are made up.

## Playing

```powershell
.\play.ps1 piraterevenge_m101cn        # WSL
.\play-win.ps1 piraterevenge_m101cn    # native Windows
```

The attract starts within a few seconds.  A credit starts the game at once
(no start button).  Left and right buttons together fire forward.

| Key | Cabinet |
| --- | --- |
| 5 / 7 | coin 1 / coin 2 (2 coins = 1 credit by default) |
| Space, Ctrl, Up | bomb |
| Left / Right | the rudder (VR) |
| Z / X | left / right button |
| F2 | test (operator menu) |
| 9 | service |
| F1 | show / hide the key help |

- **The game's own debug keys are live** (`DebugCheckKey`, read through
  its built-in SDL): 6 opens the test menu, Esc quits, Enter switches the
  I/O card off, `c` adds a coin, `t` toggles a test mode, `p` saves a
  screenshot, and `n` / `m` / space / `a` / `d` are its keyboard controls.
  The loader's keys avoid them.
- **Settings and bookkeeping** (the IGS036's NVRAM) are kept in
  `/fec_data/igs036.nv` of the rom set, next to the game's own
  `gameset.bin` (for a `.7z` set that is the cache,
  `<cache>/linux/piraterevenge_m101cn/fec_data/`).  Delete it for factory
  settings.
- **The rudder** turns while Left/Right is held and springs back to centre
  when released.  `PR_STEER=<n>` sets the turn rate in VR units per frame
  (default 3; the VR runs 0..512).
- **`PR_SCRIPT="sec:key[+],..."`** presses keys on a timetable for
  testing, e.g. `30:coin,31:coin,40:bomb`.  Keys are `coin coin2 bomb left
  right l r test service`; a press lasts 0.2 s, or with `+` until the next
  entry.

## Extracting

`igs-mkromset piraterevenge_m101cn <image>` builds the rom set;
`romtool/igs_e2000_extract.py piraterevenge_m101cn <image> <work dir>` a
full workspace (`root/`, `game/`, `sship.bin`, `loader/GECA`).

| Path | What |
| --- | --- |
| p1 ext2 | LILO map and kernel `PirateRevenge-OS-57P` |
| p2 | root filesystem (plaintext ROFS); `/pr/exec/sship` is a static decoy, `/etc/rc.d/rc0.d`..`rc7.d` the loader and executable pieces |
| p3 | IGS_SHFS game data at `/pr/linux`: `sship`, `image/`, `sound/`, `sod/` (stage files), `flc/`, `demo/` |
| p4 ext2 | `/fec_data`: `gameset.bin` (`SHIP_800` + the settings, written only) |

## How the cabinet starts it

1. The kernel mounts p3 on `/pr/linux` and p4 on `/fec_data`.
2. `/bin/HWtest` and `/mnt/GECA` build `/pr/exec/sship`
   (`IGS_ROFS_REVERSE_ENGINEERING.md`, Executable assembly).
3. `inittab` respawns `/etc/X11/IGS`, which starts XFree86 and runs
   `cd /pr/linux; ./sship` in a loop.  That is p3's unstripped copy, whose
   code is identical to the assembled one.

## The executable

A dynamically linked i386 ELF, linked like Gold Captain's: `libGL`,
`libX11`, `libXext`, `libpthread`, `libdl`, `libm` and
`/lib/libcasmcard.so`, with SDL 1.2 (X11/GLX, OSS `/dev/dsp`) and
SDL_mixer built in.  It uses the same devices (`/dev/plx/Pci9030-0`,
`/dev/roio` with the same accepted BIOS, `/dev/pccard0`), the same RC4
keys and the same command set (`IGS_E2000.md`).  Differences:

| | Gold Captain | Pirate Revenge |
| --- | --- | --- |
| Settings block (commands 13/14) | 970 words | 754 words (`SaveData`) |
| Controls | lever, GameKey, L/R | rudder, bomb, left/right buttons |
| Key bits (word 5) | | 0 right, 1 left, 2 bomb, 3/4 coin switches, 5 test, 6 service, 7 ticket |
| VR calibration | settings words 958..960 | 742..744 (`GameRudder_Init`) |
| Coin counters (2/3) | | used: `IOCardCoin_Init` zeroes them |
| Stage files | `sod/release36/`, WarZones zeroed | `sod/`, WarZones present |
| Game-flow events | | adds 2 (game start) |
| Writes | `var/` | `/fec_data/gameset.bin`, `/rf_data/seltday.dat` (fails harmlessly) |

## Loader status

Done:
- [x] Rom set: `root p2; assemble /pr/exec/sship; mount p3 /pr/linux;
      mount p4 /fec_data`.
- [x] `games/piraterevenge`: sprite-group store (6–10), NVRAM (13/14),
      coin counters (2/3), ticket dispenser (4/20/21/22), inputs, overlay.

Reconstructed IGS036 logic (details in `IGS_E2000.md`):
- [x] **Command 19, game flow**: Gold Captain's table plus 2→1 (game start
      → title) and 12→11 (last stage → game over).
- [x] **Command 5, WarZone**: the values of the stage file being loaded.
- [x] **Command 15, weapons**: Gold Captain's made-up values.
- [x] **Rudder calibration**: settings words 742–744 default to 100 / 0 /
      50 while all zero (zero divides by zero in `ControlerValue`).
- [x] **Coin counters**: past 100 they drop back to 0 once no coin has
      come for about 2 s, so a coin never falls on a wrap (the IGS036's own wrap
      is unknown).

Open:
- [ ] Real weapon values and stage parameters (need the IGS036 internal
      ROM).
- [ ] Not yet seen in play: the stage tally (`GameResult`), SOD type 1
      and 3 stages (`GameRace`, `GameStaff`), the ending, ticket payout.
- [ ] On Linux the game's absolute paths (`/fec_data/gameset.bin`,
      `/rf_data`) point at the host and fail; harmless (written, never read).
