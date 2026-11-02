# igs-loader

A loader for IGS E2100, E2200, E2000 and E1000 arcade games, with a modular plugin design
for the arcade hardware. Runs on Linux/WSL (`LD_PRELOAD`) and natively on
Windows (`igs-loader.exe`).

Games run by id from MAME-style rom sets built from your own dumps; no game
data is included.

## Supported games

| Id | Game | Machine | Linux / WSL | Windows |
| --- | --- | --- | --- | --- |
| `harpoonlagoon_m107us` | Harpoon Lagoon (M107US) | E2100 | ✓ | ✓ |
| `harpoonlagoon_m001uswa` | Harpoon Lagoon (M001US-WA) | E2100 | ✓ | ✓ |
| `harpoonlagoon_m200uswa` | Harpoon Lagoon (M200US-WA) | E2100 | ✓ | ✓ |
| `oceanparty_md105cn` | Ocean Party (MD105CN) | E2100 | ✓ | ✓ |
| `scratch_m101cn` | Scratch (M101CN) | E2100 | ✓ | ✓ |
| `heroofrobots_m100fa` | Hero of Robots (M100FA) | E2100 | ✓ | ✓ |
| `goldcaptain` | Gold Captain | E2000 | ✓ | ✓ |
| `piraterevenge_m101cn` | Pirate Revenge (M101CN) | E2000 | ✓ | ✓ |
| `gogoball2_dx` | Go Go Ball 2 DX | E2000 | ✓ | ✓ |
| `speedrider_m109cnc` | Speed Rider (M109CNC) | E2000 | ✓ | ✓ |
| `pandafamily_cn` | Panda Family (CN) | E2000 | ✓ | ✓ |
| `speeddriver2` | Speed Driver 2 | E2000 | ✓ | ✓ |
| `wedanceonline2` | We Dance Online 2 (offline) | E2000 | ✓ | ✓ |
| `percussionkids_m105cn` | Percussion Kids (M105CN) | E1000 | ✓ | ✓ |
| `percussionmaster3_m107fa` | Percussion Master 3 (M107FA) | E2200 | ✓ | ✓ |

Where the protection chip runs game logic whose code has not been dumped
(Ocean Party's odds, Scratch's prizes, the E2000 games' weapon tables and
game flow, Speed Rider's AI riders and lost archive bytes, Speed Driver 2's
screens and AI throttle, all of Percussion Kids' game flow and judging,
Percussion Master 3's screen flow), the plugin reconstructs or invents it,
so gameplay may not be completely identical to the real cabinet.

## Getting the loader

Every push builds both front ends in GitHub Actions. Download
`igs-loader-linux` or `igs-loader-windows` from a run on the **Actions** tab.
Each one is a ready-to-use `igs-loader` folder. To build it yourself, see
[Building](#building).

## Rom sets

A rom set is the cabinet's file tree as the running game sees it: the root
filesystem, the game data partition at its mount point, and the executable
where the cabinet has it at run time. A set can be:

- a folder `<id>/`, used in place (the game writes its settings into it);
- an archive `<id>.7z` or `<id>.zip`, extracted once into the cache.

The games, their settings and the hashes of the source dumps are in the
software lists `hash/e2100.xml`, `hash/e2200.xml`, `hash/e2000.xml` and
`hash/e1000.xml`.

### Building sets from your dumps

`romtool/mkromset.py` builds a set from a disk image (`.img`) or a MAME CHD
(`.chd`). It runs on Linux/WSL and needs `7z`, `unsquashfs` (squashfs-tools)
and `debugfs` (e2fsprogs). `python3-cryptography` speeds up the E2100 root
filesystem. For CHDs it also needs `chdman`, from `$CHDMAN`, `chdman=` in
`igs-loader.ini`, or PATH; a Windows `chdman.exe` works from WSL.

```bash
python3 romtool/mkromset.py --list                          # the ids and the dumps they expect
python3 romtool/mkromset.py scratch_m101cn e2100_scratch_m101cn.img
python3 romtool/mkromset.py goldcaptain goldcaptain.chd --format zip -o ~/igs-roms
```

| Option | Effect |
| --- | --- |
| `-o <dir>` | output directory (default: the first `rompath=` entry, else `roms/`) |
| `--format 7z\|zip\|dir` | archive type, or a plain folder (default `7z`, `-mx9`) |
| `--mx N` | 7z compression level |
| `--no-verify` | skip hashing the whole input image |

The input is checked against the dump's SHA-1 in the software list, and the
built set against the listed file hashes. Building takes about 4–11 minutes
per set; Hero of Robots takes about 45.

## Configuration

Copy `igs-loader.ini.example` to `igs-loader.ini` next to `run.sh` (or next
to `igs-loader.exe`) and edit it:

```ini
# where rom sets are looked for, ';'-separated
rompath=D:\games\igs\roms
# where .7z/.zip sets are extracted (each loader uses its own linux\ or windows\ subfolder)
cachepath=D:\games\igs\cache
```

Paths may be written in Windows form (`D:\...`) or WSL form (`/mnt/d/...`);
both loaders convert them, so one file serves both. The environment variables
`IGS_ROMPATH`, `IGS_CACHE` and `IGS_HASHPATH` override the file.

- **Default cache:** `~/.cache/igs-loader/linux` on Linux and
  `%LOCALAPPDATA%\igs-loader\cache\windows` on Windows.
- **Settings persist:** the extracted copy is writable, so the game's
  settings and bookkeeping are kept between runs.
- **Re-extraction resets them:** a set is extracted again when its archive
  changes, which wipes those settings.

## Usage

### Linux / WSL

One-time setup (Ubuntu 24.04; under WSL, WSLg provides the display and
PulseAudio):

```bash
sudo dpkg --add-architecture i386 && sudo apt update
sudo apt install gcc-multilib \
    libsdl1.2debian:i386 libsdl-mixer1.2:i386 libglu1-mesa:i386 \
    libxxf86vm1:i386 zlib1g:i386 libpci3:i386 libgl1:i386 libgl1-mesa-dri:i386 \
    libglx-mesa0:i386 libegl1:i386 libegl-mesa0:i386 libwayland-egl1:i386
# Hero of Robots and Speed Rider (Ogre) also need:
sudo apt install libxaw7:i386 libfontenc1:i386 libjpeg62:i386
```

- **Mesa versions:** the i386 Mesa must be the same version as the amd64 one.
- **SDL:** `libsdl1.2debian` is sdl12-compat, running on SDL2.

Then:

```bash
./run.sh --list                 # the games, and which sets were found
./run.sh --verify scratch_m101cn
./run.sh harpoonlagoon_m107us
```

From Windows, `play.ps1 <id>` runs `run.sh` in WSL.

### Windows

```powershell
.\igs-loader.exe --list
.\igs-loader.exe oceanparty_md105cn
.\igs-loader.exe --check scratch_m101cn    # load and link only, list unresolved symbols
.\hero-nfc.exe write M01-01                # Hero of Robots NFC card (experimental; also build/hero-nfc on Linux)
.\hero-nfc-gui.exe                         # the same, in a window with the card artwork
```

From a source checkout, `play-win.ps1 <id>` runs `build\win\igs-loader.exe`.
The exe needs no other DLLs. It takes the game's own system libraries from
the rom set.

### Window title and options

The window title shows the game, its status (credits, phase) and the frame
rate. Useful environment variables:

| Variable | Effect |
| --- | --- |
| `IGS_FULLSCREEN=1` | keep the game's fullscreen mode (default: windowed); on Windows an X11 game's frame is scaled to the monitor |
| `IGS_FPS_CAP=n` | frame-rate cap (default 60, `0` = off) |
| `IGS_FPS=1` | log the frame rate every 5 s |
| `IGS_TITLE=text` | window title instead of the game's name (`0` keeps the game's own) |
| `IGS_VERBOSE=1` | log every message between the game and the emulated hardware |
| `IGS_SHOT=<prefix>` | save the frame to `<prefix><n>.ppm` every `IGS_SHOT_EVERY` frames (default 600) |
| `IGS_SCRIPT="sec:key[+],..."` | press keys on a timetable, for testing |
| `IGS_WRAP="cmd"` | Linux: run the game under a command, e.g. `gdb -q --args` |
| `IGS_VSYNC=1` | Windows: also wait for vblank |

## Building

**Linux plugins** (32-bit, needs `gcc-multilib`):

```bash
./build.sh
```

This builds `build/igs_core.so`, one `.so` per machine and per game, and
`build/igs-romset`, the rom-set helper `run.sh` uses. It also unpacks
ncurses 5 from `debs/` into `lib/`, because Ubuntu 24.04 no longer ships
it. `core/font.h`, the overlay font, is regenerated with Python Pillow only
when `core/gen_font.py` is newer.

**Windows loader** (cross-compiled; needs the mingw-w64 i686 posix toolchain,
`gcc-mingw-w64-i686-posix` on Ubuntu):

```bash
./win/build-win.sh
```

This writes `build/win/igs-loader.exe`, the machine and game DLLs and the
preloaded libraries, all statically linked.

## Roadmap

- We Dance Online 2: a local game server in the plugin (accounts, QR
  login, saved profiles, rankings, versus between instances); protocol in
  `docs/WeDanceOnline2.md`.
- A graphical front end.

## Documentation

Controls and per-game settings are on the [wiki](https://github.com/Puyodead1/igs-loader/wiki).

| Doc | What |
| --- | --- |
| [IGS_LOADER.md](docs/IGS_LOADER.md) | the loader in depth: options, rom sets and software lists, architecture, the Windows loader, adding a game |
| [IGS_E2100_PROTECTION.md](docs/IGS_E2100_PROTECTION.md) | E2100 FPGA card and protection ASIC: kernel driver, boot handshake, runtime messages |
| [IGS_E2000.md](docs/IGS_E2000.md) | E2000 hardware: PLX9030 interface, IGS036 shared-RAM, IGS027A packet and IGS036 message and packet transports, command sets |
| [IGS_SOUND_CRYPTO.md](docs/IGS_SOUND_CRYPTO.md) | the FPGA asset cipher used on sounds and sprite banks |
| [IGS_E1000.md](docs/IGS_E1000.md) | E1000 hardware: the Geode GX2 board, the IGS027A's command protocol |
| [IGS_E2200.md](docs/IGS_E2200.md) | E2200: the E2100 PC side, the IGS036 framing and messages, the drum I/O board, chart encryption, the screen flow |
| [IGS_ROFS_REVERSE_ENGINEERING.md](docs/IGS_ROFS_REVERSE_ENGINEERING.md) | the disk formats: E2100 and E2200 `igs_rofs`, E2000 ROFS and IGS_SHFS, boot chains |
| [IGS_KEYS.md](docs/IGS_KEYS.md) | every key, mask and constant per game |
| [HarpoonLagoon.md](docs/HarpoonLagoon.md), [OceanParty.md](docs/OceanParty.md), [Scratch.md](docs/Scratch.md), [HeroOfRobots.md](docs/HeroOfRobots.md), [GoldCaptain.md](docs/GoldCaptain.md), [PirateRevenge.md](docs/PirateRevenge.md), [GoGoBall2.md](docs/GoGoBall2.md), [SpeedRider.md](docs/SpeedRider.md), [PandaFamily.md](docs/PandaFamily.md), [SpeedDriver2.md](docs/SpeedDriver2.md), [WeDanceOnline2.md](docs/WeDanceOnline2.md), [PercussionKids.md](docs/PercussionKids.md), [PercussionMaster3.md](docs/PercussionMaster3.md) | per-game pages: versions, controls, tunables, internals, what the plugin emulates |

## Repository layout

| Path | What |
| --- | --- |
| `core/` | the Linux `LD_PRELOAD` core: hooks for files, devices, SDL, X11/GLX, OSS audio, overlay, frame cap |
| `machines/e2100`, `machines/e2200`, `machines/e2000`, `machines/e1000` | the boards: ASIC transports, devices, protection handshakes; E1000's Geode graphics; E2200 is E2100's under its own name |
| `games/<game>/` | per-game plugins: inputs, coins, the protection chip's game logic |
| `games/heroofrobots/hero-nfc*.c` | `hero-nfc` and `hero-nfc-gui.exe` (Windows), the Hero of Robots NFC card tool (experimental; [HeroOfRobots.md](docs/HeroOfRobots.md#card-tool-experimental)) |
| `win/` | the native Windows loader: ELF loader, glibc shims on Win32, SDL 1.2 and GL layers |
| `romset/` | rom-set lookup, software-list parser, `.7z` / `.zip` reading |
| `romtool/` | the set builder and the disk-image filesystem tools |
| `hash/` | the software lists |
| `docs/` | the documentation (above) |
| `third_party/lzma/` | LZMA SDK 23.01 decoder (public domain) |
| `debs/` | ncurses 5 (i386) from Ubuntu 22.04 |
