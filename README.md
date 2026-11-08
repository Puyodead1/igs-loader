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
| `fishhunter4_m107cn` | Fish Hunter 4 (M107CN) | E2100 | ✓ | ✓ |
| `fishhunter4_m101cn` | Fish Hunter 4 (M101CN) | E2100 | ✓ | ✓ |
| `fishhunter4_m102cn` | Fish Hunter 4 (M102CN) | E2100 | ✓ | ✓ |
| `fishhunter4_m103cn` | Fish Hunter 4 (M103CN) | E2100 | ✓ | ✓ |
| `fishhunter4_m104cn` | Fish Hunter 4 (M104CN) | E2100 | ✓ | ✓ |
| `fishhunter4_m105cn` | Fish Hunter 4 (M105CN) | E2100 | ✓ | ✓ |
| `fishhunter4_m400cn` | Fish Hunter 4 (M400CN) | E2100 | ✓ | ✓ |
| `fishhunter4_m401cn` | Fish Hunter 4 (M401CN) | E2100 | ✓ | ✓ |
| `fishhunter4_m402cn` | Fish Hunter 4 (M402CN) | E2100 | ✓ | ✓ |
| `fishhunter6_m606cn` | Fish Hunter 6 (M606CN) | E2100 | ✓ | ✓ |
| `fishhunter6_m601fa` | Fish Hunter 6 (M601FA) | E2100 | ✓ | ✓ |
| `fishhunter6_m602cn` | Fish Hunter 6 (M602CN) | E2100 | ✓ | ✓ |
| `fishhunter6_m603cn` | Fish Hunter 6 (M603CN) | E2100 | ✓ | ✓ |
| `fishhunter6_m604cn` | Fish Hunter 6 (M604CN) | E2100 | ✓ | ✓ |
| `fishhunter6_m607cn` | Fish Hunter 6 (M607CN) | E2100 | ✓ | ✓ |
| `fishhunterfinal6_fish6` | Fish Hunter Final 6 (U102CN, fish6) | E2100 | ✓ | ✓ |
| `fishhunterfinal6_fish6p` | Fish Hunter Final 6 (U102CN, fish6p) | E2100 | ✓ | ✓ |
| `fishhunterfinal6_fish44` | Fish Hunter Final 6 (U102CN, fish44) | E2100 | ✓ | ✓ |
| `fishhunterplus6p_m602cn` | Fish Hunter Plus 6P (M602CN) | E2100 | ✓ | ✓ |
| `fishhunterskl_m104cn` | Fish Hunter SKL (M104CN) | E2100 | ✓ | ✓ |
| `fishhunterskl_m103cn` | Fish Hunter SKL (M103CN) | E2100 | ✓ | ✓ |
| `fishhunterskl2_m105cn` | Fish Hunter SKL2 (M105CN) | E2100 | ✓ | ✓ |
| `demonhunter_m202cn` | Demon Hunter (M202CN) | E2100 | ✓ | ✓ |
| `demonhunter_m100cn` | Demon Hunter (M100CN) | E2100 | ✓ | ✓ |
| `crazyshoot_m100cn` | Crazy Shoot (M100CN) | E2100 | ✓ | ✓ |
| `oceanparty_md105cn` | Ocean Party (MD105CN) | E2100 | ✓ | ✓ |
| `sangomario` | SangoMario (recovered; some animations and sounds lost) | E2100 | ✓ | ✓ |
| `scratch_m101cn` | Scratch (M101CN) | E2100 | ✓ | ✓ |
| `heroofrobots_m100fa` | Hero of Robots (M100FA) | E2100 | ✓ | ✓ |
| `goldcaptain` | Gold Captain | E2000 | ✓ | ✓ |
| `piraterevenge_m101cn` | Pirate Revenge (M101CN) | E2000 | ✓ | ✓ |
| `gogoball2_dx` | Go Go Ball 2 DX | E2000 | ✓ | ✓ |
| `speedrider_m109cnc` | Speed Rider (M109CNC) | E2000 | ✓ | ✓ |
| `pandafamily_cn` | Panda Family (CN) | E2000 | ✓ | ✓ |
| `speeddriver2` | Speed Driver 2 | E2000 | ✓ | ✓ |
| `wedanceonline2` | We Dance Online 2 (offline; online with `IGS_NET`) | E2000 | ✓ | ✓ |
| `percussionkids_m105cn` | Percussion Kids (M105CN) | E1000 | ✓ | ✓ |
| `percussionmaster3_m107fa` | Percussion Master 3 (M107FA) (offline; online with `IGS_NET`) | E2200 | ✓ | ✓ |
| `fishhunterskl2_m801cn` | Fish Hunter SKL2 8P (M801CN) | E2200 | ✓ | ✓ |
| `speeddriver3_m109cn` | Speed Driver 3 (M109CN) (no card play, no versus) | E3000 | ✓ | ✓ |
| `powertruck` | Power Truck (no card play, no versus) | E3000 | ✓ | ✓ |
| `muzibox` | MuziBox (no member card, no contests; Linux untested past loading) | E3000 | ? | ✓ |
| `speedrider2`, `speedrider2fa` | Speed Rider 2 (no member card, no link play; Linux untested) | E3000 | ? | ✓ |
| `speeddriver4` | Speed Driver 4 (no card play, no versus or internet race; Linux untested) | E3000 | ? | ✓ |

Where the protection chip runs game logic whose code has not been dumped
(Fish Hunter 4's, 6's, Plus 6P's, SKL's, SKL2's and Demon Hunter's catches, chance games and payouts, Ocean Party's odds, Scratch's prizes, the E2000 games' weapon tables and
game flow, SangoMario's rounds and odds, Speed Rider's AI riders and lost archive bytes, Speed Driver 2's
screens and AI throttle, all of Percussion Kids' game flow and judging,
Percussion Master 3's screen flow, Speed Driver 3's and Power Truck's screen flow, state-machine library and boot scripts (Power Truck's replays), MuziBox's screen flow, Speed Rider 2's AI riders' control, Speed Driver 4's screen flow and state-machine library, Crazy Shoot's prize odds, ported from the
developers' own stand-in in the game), the plugin reconstructs or invents it,
so gameplay may not be completely identical to the real cabinet.

The E2200 dumps of Ocean King (M015CN–M817CN), Poseidon, Full Attack, Soul of Gun
and Westtrip (MD012CN–MD108CN) are listed with `supported="no"`: their sets build
and verify, but their code is encrypted by a Sentinel HASP Envelope that only the
cabinet's HASP key can open, so the loaders refuse to run them.

SangoMario was never dumped: its files were carved from an older filesystem left in
Westtrip MD018CN's free space.  Its set is a folder of those files (no disk entry),
with stand-ins for the action banks that did not survive.

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

`igs-mkromset` (`build/igs-mkromset` on Linux, `igs-mkromset.exe` on Windows)
builds a set from a disk image (`.img`) or a MAME CHD (`.chd`, read in place).
It needs no other tools.

```bash
build/igs-mkromset --list                          # the ids and the dumps they expect
build/igs-mkromset scratch_m101cn e2100_scratch_m101cn.img
build/igs-mkromset goldcaptain goldcaptain.chd --format zip -o ~/igs-roms
```

| Option | Effect |
| --- | --- |
| `-o <dir>` | output directory (default: the first `rompath=` entry, else `roms/`) |
| `--format 7z\|zip\|dir` | archive type, or a plain folder (default `7z`, `-mx9`) |
| `--mx N` | compression level (7z, zip) |
| `--no-verify` | skip hashing the whole input image |
| `--threads N` | compression threads (default: the CPUs) |

The input is checked against the dump's SHA-1 in the software list, and the
built set against the listed file hashes. `romtool/mkromset.py` is the
Python reference implementation (Linux/WSL, with `7z`, `unsquashfs`,
`debugfs` and `chdman`); both build the same sets.

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

A game's own variables go in a section named after its id; both loaders
export them for that game only (variables already set in the environment
win). `IGS_INI` names another settings file.

```ini
[oceanparty_md105cn]
OCEAN_COIN=20
IGS_FPS_CAP=0
```

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

### GUI

`igs-loader-gui` (`igs-loader-gui.exe` on Windows, `build/igs-loader-gui` on
Linux, which needs `libsdl2-2.0-0`) is a launcher in the style of a MAME
front end for the loader of its own platform:

- the game list with set status, search, machine filter, sorting and
  clones; keyboard and gamepad navigation (Enter or A launches);
- a page per game: details, the snapshot (`snap/<id>.png`, or the newest
  frame the Snapshots option saved), the game's options from the software
  list, online play with an optional local igs-online server, the Hero of
  Robots NFC card tool;
- Launch, Verify and Stop, one game at a time, with the log live (also in
  `logs/<id>.log`);
- global options (`IGS_*`), settings (rompath, cachepath, hashpath,
  chdman), set building from a disk image, cache sizes and reset.

Everything is stored in `igs-loader.ini`: the game's options in its
section, so command-line runs use them too; the launcher's own settings
in `[gui]`.

### Game data

`igs-assets` (`build/igs-assets`, `igs-assets.exe`) lists, dumps, checks
and extracts the games' own data formats: image and action banks, fonts,
models, archives, charts, settings and bookkeeping, to PNG, WAV, OBJ and
JSON. Encrypted E2100 assets are decrypted with the set's mask. The formats
are on the [wiki](https://github.com/Puyodead1/igs-loader/wiki/Formats).

```powershell
.\igs-assets.exe formats
.\igs-assets.exe extract <cache>\windows\fishhunter4_m107cn\Fish\action\02_nemo.rom -o out
.\igs-assets.exe scan <cache>\windows\speeddriver2    # check every known file in a set
```

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
| `IGS_ROTATE=90\|180\|270` | turn the frame clockwise and fit it to the window (a rotated cabinet monitor; MuziBox's list entry sets 90) |
| `IGS_SCRIPT="sec:key[+],..."` | press keys on a timetable, for testing |
| `IGS_NET=host[:port]` | online games: connect to a local stand-in server (igs-online; We Dance, Percussion Master 3) instead of staying offline; nothing else reaches the network |
| `IGS_WRAP="cmd"` | Linux: run the game under a command, e.g. `gdb -q --args` |
| `IGS_VSYNC=1` | Windows: also wait for vblank |

## Building

**Linux plugins** (32-bit, needs `gcc-multilib`):

```bash
./build.sh
```

This builds `build/igs_core.so`, one `.so` per machine and per game,
`build/igs-romset`, the rom-set helper `run.sh` uses, and
`build/igs-mkromset`, the set builder, `build/igs-assets`, the game data tool, and `build/igs-loader-gui` when
`libsdl2-dev` is installed. It also unpacks
ncurses 5 from `debs/` into `lib/`, because Ubuntu 24.04 no longer ships
it. `core/font.h`, the overlay font, is regenerated with Python Pillow only
when `core/gen_font.py` is newer.

**Windows loader** (cross-compiled; needs the mingw-w64 i686 posix toolchain,
`gcc-mingw-w64-i686-posix` and `g++-mingw-w64-i686-posix` on Ubuntu):

```bash
./win/build-win.sh
```

This writes `build/win/igs-loader.exe`, the machine and game DLLs, the
preloaded libraries, `igs-mkromset.exe`, `igs-assets.exe` and `igs-loader-gui.exe`, all
statically linked.

## Roadmap

- Percussion Master 3 online: the dragon fight in song select (offered
  and listed, never landed on by scripted runs) and the world dragon not
  yet seen in the game.

## Documentation

The documentation is on the [wiki](https://github.com/Puyodead1/igs-loader/wiki).

| Page | What |
| --- | --- |
| [Games](https://github.com/Puyodead1/igs-loader/wiki/Games) | per game: versions, controls, tunables, internals, what the plugin emulates |
| [Loader](https://github.com/Puyodead1/igs-loader/wiki/Loader) | the loader in depth: options, rom sets and software lists, architecture, the Windows loader, adding a game |
| [E1000](https://github.com/Puyodead1/igs-loader/wiki/E1000), [E2000](https://github.com/Puyodead1/igs-loader/wiki/E2000), [E2100 Protection](https://github.com/Puyodead1/igs-loader/wiki/E2100-Protection), [E2200](https://github.com/Puyodead1/igs-loader/wiki/E2200) | the boards, protection chips and their protocols |
| [Disk Formats](https://github.com/Puyodead1/igs-loader/wiki/Disk-Formats) | E2100 and E2200 `igs_rofs`, E2000 ROFS and IGS_SHFS, boot chains |
| [Sound Crypto](https://github.com/Puyodead1/igs-loader/wiki/Sound-Crypto) | the FPGA asset cipher used on sounds and sprite banks |
| [Keys](https://github.com/Puyodead1/igs-loader/wiki/Keys) | every key, mask and constant per game |
| [Formats](https://github.com/Puyodead1/igs-loader/wiki/Formats) | the games' own data formats |

## Repository layout

| Path | What |
| --- | --- |
| `core/` | the Linux `LD_PRELOAD` core: hooks for files, devices, SDL, X11/GLX, OSS audio, overlay, frame cap |
| `machines/e2100`, `machines/e2200`, `machines/e2000`, `machines/e1000` | the boards: ASIC transports, devices, protection handshakes; E1000's Geode graphics; E2200 is E2100's under its own name |
| `games/<game>/` | per-game plugins: inputs, coins, the protection chip's game logic |
| `games/heroofrobots/hero-nfc*.c` | `hero-nfc` and `hero-nfc-gui.exe` (Windows), the Hero of Robots NFC card tool (experimental; [wiki](https://github.com/Puyodead1/igs-loader/wiki/Hero-of-Robots#card-tool-experimental)) |
| `win/` | the native Windows loader: ELF loader, glibc shims on Win32, SDL 1.2 and GL layers |
| `gui/` | `igs-loader-gui`: Dear ImGui on Win32 + OpenGL 3 (Windows) or SDL2 + OpenGL 3 (Linux) |
| `romset/` | rom-set lookup, software-list parser, `.7z` / `.zip` reading |
| `mkromset/` | the set builder, `igs-mkromset` (a library and its CLI) |
| `assets/` | the game data formats, `igs-assets` (libigsa, a module per format, and its CLI) |
| `romtool/` | the Python reference builder and the disk-image filesystem tools |
| `hash/` | the software lists |
| `third_party/lzma/` | LZMA SDK 23.01 subset (public domain) |
| `third_party/zlib/` | zlib 1.3.1 and minizip (zlib licence) |
| `third_party/libchdr/` | libchdr (BSD-3-Clause) with zstd 1.5.7 (BSD) and dr_flac (public domain / MIT-0) |
| `third_party/lzokay/` | lzokay's LZO1X decoder, commit `db2df1f` (MIT) |
| `third_party/imgui/` | Dear ImGui 1.92.9b (MIT) |
| `third_party/stb/` | stb_image 2.28 and stb_image_write 1.16 (public domain / MIT) |
| `debs/` | ncurses 5 (i386) from Ubuntu 22.04 |
