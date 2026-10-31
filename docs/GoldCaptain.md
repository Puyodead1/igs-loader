# Gold Captain (E2000)

The first E2000 game brought up.  Product string `GoldCaptian-E2000-v1-e65`;
the executable was built 12 Mar 2009 and the kernel is `2.4.31-IGS_V1.0`
(1 Dec 2008).

**Status:**
- Fully extracted from the stock image.
- **Plays in the loader** (machine `e2000`, plugin `goldcaptain`), as of
  2026-10-30:
  - attract, sound, coins and credits;
  - captain select, stage, boss, stage-clear tally;
  - the result screen with ticket payout, then back to attract.
- The IGS036's own game logic (game flow, weapon table, stage parameters)
  is in its internal ROM, which is not dumped.  The plugin reconstructs it
  from the executable (see "Loader status").  The weapon values are made up.

## Playing

```powershell
.\play.ps1 goldcaptain        # WSL
.\play-win.ps1 goldcaptain    # native Windows
```

Loading takes about 50 s before the attract screen.  The game asks for
fullscreen; the Windows loader keeps it windowed unless `IGS_FULLSCREEN=1`.

| Key | Cabinet |
| --- | --- |
| 5 / 6 | coin 1 / coin 2 (2 coins = 1 credit by default) |
| Space, Ctrl, Up | GameKey: start / fire |
| Left / Right | the lever (VR) |
| Z / X | L / R button |
| F2 | setting (test menu) |
| 9 | service |
| F1 | show / hide the key help |

- **Settings and bookkeeping** are kept in `/GoldCaptain/var/igs036.nv` of
  the rom set (the IGS036's NVRAM; for a `.7z` set that is the cache,
  `<cache>/linux/goldcaptain/GoldCaptain/var/`).  Delete the file for
  factory settings.
- **On Windows** the NVRAM is in `<cache>\windows\goldcaptain\GoldCaptain\var\`.
- **The lever** turns while Left/Right is held, about 1.4 s from centre to
  full lock, and springs back to centre when released.  `GC_STEER=<n>`
  sets the turn rate in VR units per frame (default 3; the VR runs
  0..512).
- **`GC_SCRIPT="sec:key[+],..."`** presses keys on a timetable for testing,
  for example `62:coin,63:coin,66:start`.  Keys are `coin start left right
  l r setting service`; a press lasts 0.2 s, or with `+` until the next
  entry.

## Extracting

For a rom set use `romtool/mkromset.py goldcaptain <image>` (see
`IGS_LOADER.md`).  For a full workspace, with the kernel and loaders too:

```
python3 romtool/igs_e2000_extract.py goldcaptain e2000_goldcaptain.img <work dir>
```

This takes the assembly constants from `hash/e2000.xml` and writes:

| Path | What |
| --- | --- |
| `root/` | p2 root filesystem (plaintext ROFS): bash, XFree86 4, X11 libs, `/lib/libdb*.a` pieces, `/bin/csh` stage-1 loader, `libcasmcard.so` |
| `game/` | p3 IGS_SHFS: `flc/` FLIC movies, `image/**/*.rom`, `sound/`, `sod/` (stage scripts), `keyindat/` (input-method data), `net/` (pppd/pppoe), `system/` fonts |
| `GoldCaptain.bin` | the reassembled executable (1,506,192 bytes) |
| `loader/GECA` | the stage-2 loader |
| `p1/` | LILO map and kernel; `goldcaptain-os-gzip-payload.bin` is the decompressed kernel (load address `0xC0100000`) |
The formats and the boot chain are described in
`IGS_ROFS_REVERSE_ENGINEERING.md` ("E2000"); the constants are in
`IGS_KEYS.md`.

## How the cabinet starts it

1. The kernel mounts p3 on `/GoldCaptain` and p4 on `/GoldCaptain/var`.
2. It builds `/Game/GoldCaptain` with `csh` and `GECA` (see above).
3. `inittab` respawns `/etc/X11/IGS`, which:
   - starts XFree86 and mwm, and hides the cursor;
   - runs `cd /GoldCaptain; ./GoldCaptain` in a loop.

## The executable

It is a dynamically linked i386 ELF.  It needs `libGL`, `libX11`,
`libXext`, `libpthread`, `libdl` and `/lib/libcasmcard.so` (a smart-card
reader library, SLE4404/I2C memory cards).

Statically linked into it:
- **SDL 1.2**, X11 video with GLX through `dlopen("libGL.so.1")`, and OSS
  `/dev/dsp` audio;
- **SDL_mixer**, with MikMod and Ogg.

The loader core's SDL hooks therefore do not apply.  This game needs X11
and GLX hooks, plus an OSS `/dev/dsp` emulation.

## Hardware interface (differs from E2100)

| Device | Use | Code |
| --- | --- | --- |
| `/dev/plx/Pci9030-0` | **main I/O and protection.**  PLX SDK driver for a PCI9030 bridge (vendor `0x10B5`, device `0x9030`).  BAR2 is message RAM, BAR3 a mailbox (`app/c/shareram_operator.c`); the peer is an IGS036 protection CPU.  Protocol in `IGS_E2000.md` | `sub_80CCA90`, `sub_80CD1F0`, PLX SDK `sub_813CE10`/`sub_813CFB0`/`sub_813D250` |
| `/dev/roio` | BIOS fingerprint: ioctl `0xFC` reads a ROM byte (address `^0x1FB8408E`, result `^0xB8`), compared with a table at `0x8173320`, with random decoy reads around it | `sub_813B760` |
| `/dev/pccard0` | opened at start-up.  Only used when the BIOS check fails: decoy ioctls, then challenge ioctls `100/110/150/160` whose answers are `0xC8/0xD0/0xA8/0xB0 <<24 \| arg` | `sub_8051490`, `sub_813BC30` |
| `/dev/mixer`, `/dev/dsp` | OSS audio, through SDL | `sub_806AFE0` |
| `ioperm` | one call, inside SDL (DGA/fbcon) | |

## Loader status

Done:
- [x] `machines/e2000`: PLX SDK ioctls, BAR mapping, the shared-RAM
      transport with RC4-wrapped keys, `/dev/roio`, `/dev/pccard0`.
- [x] Core: X11 keys, the GLX swap hook, OSS `/dev/dsp` → PulseAudio, the
      Xlib lock and SIGSEGV-handler fixes (`IGS_LOADER.md`).
- [x] Windows: Xlib/GLX on WGL, `/dev/dsp` on waveOut (`IGS_LOADER.md`,
      Windows).
- [x] `games/goldcaptain`: the sprite-group store (commands 6–10), NVRAM
      (13/14), inputs, coins, the overlay.

Reconstructed IGS036 logic (play-tested 2026-10-30; details in
`IGS_E2000.md`, Commands):
- [x] **Command 19, game flow** (`flow_next`): rebuilt from the 12 scene
      handlers of `sub_8075BE0`.  One game runs: captain select → stage
      progress (scene 3) → area pick → stage → scene 3 → area pick →
      boss → stage-clear tally → scene 3 → result → attract.
- [x] **Command 5, WarZone**: read from the older `sod/` stage copies.
      The `sod/release36/` files the game loads have the section zeroed.
      Unused zones answer −1 (`sodReader_IsWarZoneEmpty`).
- [x] **Command 15, weapons**: the field meanings come from Pirate
      Revenge's symbols.  The values are chosen per class (player,
      enemy/boss, pick-up), not the IGS036's.  Pirate Revenge also gets
      the table from its IGS036, so no real values exist on disk.
- [x] **Commands 4/20/21/22, ticket dispenser**: pays one ticket per
      0.2 s.
- [x] **Wheel calibration**: NVRAM words 958–960 default to 100 / 0 / 50
      while all zero (the factory value pins the steering).

Open:
- [ ] Real weapon values (would need the IGS036 internal ROM).  Balance is
      a guess: rank D and 0 gold in the scripted runs, which hold fire
      but do not aim.
- [ ] Scene 6 (SOD type 1) and scene 9 (type 3) stages, and the
      long mode (`dword_86CFF40`), are not yet seen in play.
- [ ] Map the remaining button bits (3–6) with the I/O test menu.
