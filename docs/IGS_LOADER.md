# IGS loader

The loader runs an IGS game's **stock binary** directly on Linux/WSL, with
no VM, no kernel and no guest X server, in the style of `lindbergh-loader`.
An `LD_PRELOAD` core plus plugins stand in for the arcade hardware; on
Windows, `igs-loader.exe` runs the same binaries natively (see Windows).
Nothing is modified: the game runs from a **rom
set**, the cabinet's file tree as the running game sees it, built from the
original disk image (see Rom sets).

Status (2026-10-29), under WSLg:

| Game | Machine | State |
| --- | --- | --- |
| Harpoon Lagoon M107US | E2100 | playable, 4 keyboard stations |
| Harpoon Lagoon M001US-WA, M200US-WA | E2100 | playable (same plugin, variant `wa`); see `HarpoonLagoon.md`, Versions |
| Ocean Party md105cn | E2100 | playable, 1 keyboard player, re-implemented game logic |
| Scratch m101cn | E2100 | playable, 1 keyboard player, register-table transport, made-up prizes, about 20 fps (`Scratch.md`) |
| Hero of Robots m100fa | E2100 | plays a whole game: attract, coins, start, fights, card debit and dispense, game over; request/response transport, emulated smart card (RSA), dispenser and QR reader always ready, no card scanning; slow start-up from a Windows drive (`HeroOfRobots.md`) |
| Gold Captain | E2000 | plays a full game: attract, captain select, stages, boss, result and tickets; IGS036 game logic reconstructed, weapon values made up (`GoldCaptain.md`) |

They run at about 56–60 fps (60 fps cap) on Mesa llvmpipe, with continuous
sound through PulseAudio (WSLg).

## Usage

```powershell
.\play.ps1 --list                   # the games and their sets
.\play.ps1 harpoonlagoon_m107us
.\play.ps1 oceanparty_md105cn
.\play.ps1 heroofrobots_m100fa      # needs libxaw7:i386 libfontenc1:i386
```

or in WSL:

```bash
./run.sh <id>       # --list, --verify <id>
```

`<id>` is a software name from the software lists (`hash/*.xml`, see Rom
sets): `harpoonlagoon_m107us`, `harpoonlagoon_m001uswa`,
`harpoonlagoon_m200uswa`, `oceanparty_md105cn`, `scratch_m101cn`,
`heroofrobots_m100fa`, `goldcaptain`.  The per-game controls are on each
game's page (`HarpoonLagoon.md`, `OceanParty.md`).

| Variable | Effect |
| --- | --- |
| `IGS_FULLSCREEN=1` | keep the game's fullscreen request (default: windowed) |
| `IGS_FPS_CAP` | frame-rate cap (default 60, `0` = off; see Audio) |
| `IGS_FPS=1` | log the frame rate every 5 s |
| `IGS_VERBOSE=1` | log every ASIC message from the game |
| `IGS_AUDIO_CHUNK` | audio buffer in samples (default: the game's) |
| `IGS_AUDIO_LOG=1` | log the game's SDL_mixer calls |
| `SDL_VIDEODRIVER` | default `wayland`, or the game's `video_driver` feature; `x11` with sdl12-compat hits a GLX error on WSLg (see below) |
| `SDL_AUDIODRIVER` | default `pulseaudio` (WSLg's server), or the game's `audio_driver` feature |
| `IGS_SHOT=<prefix>` | write the frame to `<prefix><n>.ppm` every `IGS_SHOT_EVERY` frames (default 600) |
| `IGS_WRAP="cmd"` | run the game under a command, e.g. `gdb -q --args` or `strace -f -o log` |
| `IGS_DSP_LATENCY_MS` | OSS `/dev/dsp` → PulseAudio target latency (default 60) |
| `IGS_X11_LOCKS=1` | keep libX11's display locks (see E2000 below) |
| `IGS_SCRIPT="sec:key[+],..."` | press keys on a timetable from the first frame, through the game plugin's key handler: an SDL 1.2 key number, a character, or `up down left right space enter ctrl alt shift esc f1..f12`; 0.2 s, or with `+` until the next entry.  For testing |
| `IGS_NULL_DEVS="/dev/ttyS0 ..."` | device paths that open as a null port: writes are dropped, reads fail with `EAGAIN` (for optional peripherals such as lamp boards; usually set by the game's `env` feature).  Peripherals that must answer are the game plugin's (E2100 `serial`) |
| `IGS_X11_GRAB=1` | keep the game's X keyboard/pointer grabs, pointer warps and `XAutoRepeatOff` (default: no-ops) |
| `IGS_TITLE` | window title text in place of the game's description; `0` keeps the game's own title.  The title reads `name \| status \| fps`, refreshed every second; the status comes from the game plugin (credits, phase or state) |
| game plugins | their own, e.g. `OCEAN_BET_SECONDS`, `GC_SCRIPT`, `HERO_CARD_CREDIT` (see the game's page) |
| `IGS_ROMPATH`, `IGS_CACHE`, `IGS_HASHPATH` | override `rompath=`, `cachepath=`, `hashpath=` of `igs-loader.ini` (see Rom sets) |

## Setup (once)

WSL Ubuntu 24.04 with WSLg.  The 32-bit runtime libraries:

```bash
sudo apt install libsdl1.2debian:i386 libsdl-mixer1.2:i386 libglu1-mesa:i386 \
    libxxf86vm1:i386 zlib1g:i386 libpci3:i386 libgl1:i386 libgl1-mesa-dri:i386 \
    libglx-mesa0:i386 libegl1:i386 libegl-mesa0:i386 libwayland-egl1:i386
# Hero of Robots (Ogre): also
sudo apt install libxaw7:i386 libfontenc1:i386 libjpeg62:i386
./build.sh
```

`build.sh` needs `gcc-multilib` and Python Pillow (for the overlay font).  It
does three things:

1. Builds `build/igs_core.so`, one `.so` per machine and one per game, and
   `build/igs-romset` (native 64-bit: software lists, rompath, `.7z`/`.zip`
   sets; see Rom sets).
2. Generates `core/font.h` from DejaVu Sans Mono Bold.
3. Unpacks ncurses 5 into `lib/` (`debs/`, from Ubuntu 22.04; 24.04 no
   longer has it).

Then build the rom sets from the disk images (`romtool/mkromset.py`, next
section) and point `rompath=` in `igs-loader.ini` at them.

Notes:
- `libsdl1.2debian` is `sdl12-compat` on SDL2.
- The i386 Mesa must be the same version as the amd64 one.  apt upgrades
  both together; this system uses the kisak PPA.

## Rom sets

The loader runs games by id, MAME style.  Three pieces:

- **Software lists**, `hash/<machine>.xml` (`e2100.xml`, `e2000.xml`), modelled
  on MAME's `hash/*.xml`.  One `<software name="<id>">` per dump, with its
  description, year, publisher, version, `cloneof` for clones (informational,
  like MAME), the loader settings as `<feature>`s, the source disk
  (`<disk name sha1>`, the SHA-1 of the raw image, which is chdman's data
  SHA-1) and the hashes of key files of the built set (`<rom name size
  sha1>` in `<dataarea name="files">`).  The list's name is the machine
  (`machines/<name>`, `build/<name>.so`, `build/win/<name>.dll`).  Ids are
  unique across lists.
- **Rom sets**: a set is the cabinet's file tree as the running game sees
  it.  The root fs (p2) at `/`; the game data partition (p3) at its mount
  point (`/Fish`, `/OceanParty`, `/Scratch`, `/Robots`, `/GoldCaptain`); p4
  where the game needs it (Hero of Robots' `rw_data`); the executable where
  the cabinet has it at run time (`/Fish.bin/fish`, `/Scratch.bin/swin`
  decrypted, `/Game/GoldCaptain` reassembled).  A set is a folder `<id>/`,
  or an archive `<id>.7z` / `<id>.zip` with the tree at its root or under one
  top folder `<id>/`.  Symlinks are stored as links; absolute ones are made
  relative to the set root.
- **The rompath**: `IGS_ROMPATH`, else `rompath=` in `igs-loader.ini` next
  to `run.sh` (gitignored; copy `igs-loader.ini.example`), else `roms/`.  A
  `;`-separated list (`:` also works on Linux); paths may be written as
  `T:\...` or `/mnt/t/...` and each side converts.  For each entry the loader
  tries `<dir>/<id>/`, `<dir>/<id>.7z`, `<dir>/<id>.zip`, in that order.

A **folder** set is used in place (the game writes its settings into it).
An **archive** set is extracted once into the cache,
`${XDG_CACHE_HOME:-~/.cache}/igs-loader/linux/<id>` in WSL and
`%LOCALAPPDATA%\igs-loader\cache\windows\<id>` on Windows.  `cachepath=`
(or `IGS_CACHE`) moves it; one setting serves both loaders, since each
extracts into its own `linux`/`windows` subfolder (the stamps hold host
paths, and Windows turns symlinks into copies).  A stamp file, `.igs-romset` (archive path, size
and mtime), makes it extract again when the archive changes.  The cache is
writable, so the game's settings and bookkeeping persist between runs; **a
re-extraction resets them** (copy files out of the cache first if they
matter).  Extraction takes about 20–35 s for a Harpoon set.

```
$ run.sh --list
id                         list   year  description              clone of               set
goldcaptain                e2000  2009  Gold Captain                                    7z  /mnt/t/.../roms/goldcaptain.7z
harpoonlagoon_m107us       e2100  2013  Harpoon Lagoon (M107US)                         7z  ...
harpoonlagoon_m001uswa     e2100  2013  Harpoon Lagoon (M001US-WA)  harpoonlagoon_m107us  7z  ...
$ run.sh --verify scratch_m101cn       # the <rom> hashes, like mame -verifyroms
```

`igs-loader.exe --list` and `--verify <id>` do the same on Windows.

### Software list entries

```xml
<software name="scratch_m101cn">
    <description>Scratch (M101CN)</description>
    <year>2012</year>
    <publisher>IGS</publisher>
    <info name="version" value="M101CN"/>
    <part name="hdd" interface="igs_hdd">
        <feature name="plugin" value="scratch"/>
        <feature name="data" value="/Scratch"/>
        <feature name="binary" value="SWIN"/>
        <feature name="libs" value="lib/libGLEW.so.1.6 lib/libzzip-0.so.13"/>
        <feature name="env" value="force_glsl_version=130 IGS_NULL_DEVS=/dev/ttyS0"/>
        <feature name="rofs_profile" value="scratch"/>
        <feature name="rofs_key" value="SwinV1.00Scratch"/>
        <feature name="asset_mask" value="e4be1b4e7f081654fa6b81698b039a06"/>
        <feature name="build" value="root p2; decrypt /Scratch/swin /Scratch.bin/swin 1801508; copy /Scratch/asic36.bin /Scratch.bin/asic36.bin; mount p3 /Scratch"/>
        <diskarea name="harddriv">
            <disk name="e2100_scratch_m101cn" sha1="22f0c8a9a6c1bce7fae7febc3cb9f84730164e8a"/>
        </diskarea>
        <dataarea name="files">
            <rom name="/Scratch.bin/swin" size="1801508" sha1="..."/>
        </dataarea>
    </part>
</software>
```

| Feature | Meaning |
| --- | --- |
| `plugin` | the game plugin (`build/<plugin>.so`, `build/win/<plugin>.dll`) |
| `variant` | a build the game plugin treats differently (exported as `IGS_VARIANT`), e.g. `wa` for Harpoon Lagoon's US-WA settings layout |
| `data` | the cabinet path of the game's working directory, e.g. `/Fish`, `/Robots/exe` |
| `binary` | the executable: relative to `data` (the cabinet runs `./FH` there) or a cabinet path |
| `libs` | single libraries of the game's own (cabinet paths, or relative to `data`), exposed after the host's; the rest of the game's bundled libraries (SDL) stay hidden |
| `env` | per-game environment, `NAME=value ...` (Mesa driconf workarounds, `IGS_NULL_DEVS`) |
| `preload` | extra libraries from `build/` to preload (WSL) |
| `video_driver` / `audio_driver` | SDL drivers when the default (`wayland` / `pulseaudio`) does not apply |
| `asset_mask` | the asset-cipher mask (exported to the machine as `IGS_ASSET_MASK`; also Scratch's executable mask) |
| `rofs_profile` / `rofs_key` | E2100 root filesystem variant (`romtool/igs_rofs.py` profile) and its key literal |
| `geca_sector` … `trim_step` | E2000 executable assembly (`romtool/igs_e2000_extract.py`) |
| `build` | the builder's recipe (below) |

The description is the window title (exported as `IGS_NAME`).  The `<info
name="usage">` line points at the game's page and its plugin tunables.
Keys and constants are also tabulated in `IGS_KEYS.md`.

The lists are read by a small strict parser in C (`romset/softlist.c`, no
XML library) and by `romtool/softlist.py` (the Python tools).  In WSL,
`run.sh` gets the entry from `build/igs-romset --env <id>`, which prints the
features as shell-quoted `KEY='value'` lines (upper case: `DATA`, `BINARY`,
`LIBS`, ...) plus `ID`, `NAME`, `MACHINE`, `PARENT` and `SET` (the resolved
set directory).

### Building sets: `romtool/mkromset.py`

```bash
python3 romtool/mkromset.py --list
python3 romtool/mkromset.py scratch_m101cn e2100_scratch_m101cn.img
python3 romtool/mkromset.py goldcaptain goldcaptain.chd --format zip -o /tmp/sets
```

`mkromset.py <id> <image.chd|image.img> [-o <dir>] [--format 7z|zip|dir]
[--mx N] [--no-verify]`.  It runs in WSL (needs `7z`, `unsquashfs`,
`debugfs`; `python3-cryptography` speeds up the E2100 root fs).

1. **Input.**  A `.img` is hashed and compared with the list's disk SHA-1
   (a warning on mismatch; `--no-verify` skips the full read).  For a
   `.chd` the header's data SHA-1 is compared, then `chdman extracthd`
   writes a temporary image (chdman: `$CHDMAN`, `chdman=` in
   `igs-loader.ini`, or PATH; a Windows chdman is fine from WSL).
2. **The recipe**, the entry's `build` feature: `;`-separated steps that
   follow the cabinet's boot.

   | Step | Does |
   | --- | --- |
   | `root p2` | the root fs: E2100 `igs_rofs` (`romtool/igs_rofs.py`, the entry's `rofs_profile`), E2000 plaintext ROFS (`igs_rofs24.py`) |
   | `decrypt SRC DST SIZE` | FPGA-cipher decrypt `SRC` into `DST` with `asset_mask`, keep `SIZE` bytes (Scratch's `/bin/unrar /Scratch/swin /Scratch.bin/swin`) |
   | `copy SRC DST` | copy inside the tree (Scratch's `/etc/X11/IGS` copies `asic36.bin`) |
   | `assemble DST` | E2000: rebuild the split executable at `DST` |
   | `mount pN DIR` | partition N replaces `DIR`'s contents, as the mount hides them: SquashFS (E2100 p3, `unsquashfs -o`), IGS_SHFS (E2000 p3, `igs_shfs.py`) or ext2/3 (p4, `debugfs rdump`; `lost+found` left out) |

   Only the partitions a recipe names are read, straight from the image
   (no partition copies).
3. **Fix-ups.**  Absolute symlinks become relative to the set root.
4. **Check** the `<rom>` hashes and the executable.
5. **Pack**: `7z a -mx9 -snl` (default), zip (deflate, Info-ZIP symlinks),
   or a folder.  Output goes to `-o`, else the first `rompath=` entry of
   `igs-loader.ini`, else `roms/`; the staging tree is
   `<output>/.<id>.stage` and is deleted when packed.

Set sizes (`7z -mx9`, 7-Zip 23.01) and build times on this machine:

| Id | `.7z` | Extracted | Build |
| --- | ---: | ---: | ---: |
| `harpoonlagoon_m107us` | 256 MB | 0.76 GB | 4 min |
| `harpoonlagoon_m001uswa` | 256 MB | 0.76 GB | 4 min |
| `harpoonlagoon_m200uswa` | 256 MB | 0.76 GB | 4 min |
| `oceanparty_md105cn` | 252 MB | 0.82 GB | 4.5 min |
| `scratch_m101cn` | 122 MB | 0.58 GB | 5 min |
| `heroofrobots_m100fa` | 2,083 MB | 5.8 GB | 44 min |
| `goldcaptain` | 518 MB | 1.2 GB | 11 min |

### Reading archives (`romset/`)

One portable C implementation, compiled into `build/igs-romset` (WSL) and
into `igs-loader.exe`:

- **zip:** stored and deflated entries, zip64; a small streaming inflate
  (`romset/zip.c`), no zlib.  Unix modes and Info-ZIP symlinks are kept.
- **7z:** the LZMA SDK 23.01 C decoder (public domain,
  `third_party/lzma/`) parses the archive; the file data are decoded **as a
  stream** (`romset/sevenzip.c`): each folder's coder graph becomes a chain
  of pull decoders (pack stream → LZMA/LZMA2 → BCJ, Delta or BCJ2 with its
  four inputs), never a whole solid block in memory.  Memory is the
  dictionary sizes plus small buffers: about 70 MB for a `-mx9` set
  (64 MiB dictionary), so the 32-bit Windows loader extracts in-process.
  Methods: Copy, LZMA, LZMA2, BCJ, BCJ2, Delta (what 7-Zip uses on these
  files).  PPMd, BZip2, ARM filters and encryption are refused with a
  message.  CRCs are checked.
- **Symlinks:** recreated on Linux (an absolute target is rewritten relative
  to the set root).  On Windows they are materialised as copies of their
  targets, resolved inside the set: file links copied, directory links
  copied as trees, links into themselves and dangling links skipped.
- Entry names with `..`, absolute names and drive letters are refused.
## Architecture

```
tools/
  igs-loader/                  its own git repository
    hash/<machine>.xml         software lists: one entry per game (ids, features, hashes)
    igs-loader.ini(.example)   rompath=, cachepath= (the .ini is local, not in git)
    roms/                      default rompath (not in git)
    romset/                    rom sets: lists, rompath, cache, zip + streaming 7z (C)
    romtool/                   mkromset.py (set builder), igs_rofs.py, igs_rofs24.py,
                               igs_shfs.py, igs_fpga_decrypt.py, igs_e2000_extract.py,
                               softlist.py, IgsRofsExtractor/ (the C# original)
    third_party/lzma/          LZMA SDK 23.01 C decoder subset (public domain)
    core/        igs.h core.c  LD_PRELOAD library (Linux/SDL glue)
    machines/e2100/            e2100.h (game API) e2100.c (the board)
    machines/e2000/            e2000.h e2000.c, libcasmcard.c (stub library)
    games/harpoonlagoon/       harpoonlagoon.c
    games/oceanparty/          oceanparty.c
    games/scratch/             scratch.c
    games/heroofrobots/        heroofrobots.c, libcasmcard.c (reader + card), card.h
    games/goldcaptain/         goldcaptain.c
    build/                     igs_core.so  e2100.so  e2000.so  <game>.so  libcasmcard.so
                               libcasmcard-hero.so  igs-romset
```

There are three layers, like RetroArch's frontend and cores.  Each talks to
the next only through a table of function pointers, never through shared
symbols.

| Layer | Owns | Today |
| --- | --- | --- |
| **core** (`igs_core.so`, the `LD_PRELOAD` library) | libc/SDL interposition, routing device files to the machine, MMIO traps, libc fixes, frame pacing, audio, the overlay renderer | the only Linux/SDL/GL-specific code |
| **machine** (`$IGS_MACHINE`) | the board: device files, registers, DMA, the protection transport | `e2100`: `/dev/pccard0`, BARs, DMA, the asset cipher (mask from the software list), the ASIC handshake, and three runtime transports: batch framing (Harpoon, Ocean Party), the register table (Scratch) and request/response (Hero of Robots), chosen by the game plugin's `transport`; serial ports with a peripheral the game plugin answers for.  `e2000`: the PLX PCI9030 card (`/dev/plx/*`), shared-RAM transport to the IGS036 with RC4-wrapped keys, `/dev/roio`, `/dev/pccard0` |
| **game** (`$IGS_GAME`, loaded by the machine) | what that game's protection firmware does: channel cipher, message set, game logic, input, its overlay | `harpoonlagoon` (XOR channel, credits/buttons), `scratch` (register-table transport: inputs, credits, ticket deals), `oceanparty` (RC4 channel, the whole betting game, player panel), `heroofrobots` (request/response: RC4 channel, state pointer, settings, inputs, RSA key parts; QR reader and card dispenser on the serial ports; plus a preloaded `libcasmcard` with the smart card), `goldcaptain` (IGS036 commands: sprite-group store, NVRAM, game flow, WarZones, weapon table, ticket dispenser, inputs) |

**Interfaces:**
- **Core ↔ machine:** `core/igs.h`.
  - The core provides `struct igs_core`: log, config, time, a placeholder
    fd, `mmio_map` (trapped register windows with read/write callbacks), and
    the overlay primitives `fill`/`text`.
  - A machine exports `igs_machine_entry()`, which returns `struct
    igs_machine`: `handles`/`open`/`ioctl`/`mmap`/`close` for its device
    files, optionally `read`/`write` (serial ports; termios calls arrive at
    `ioctl` as `TCGETS`/`TCSETS`/`TCFLSH`), plus `key`, `draw` and `status`
    (a short line for the window title, which E2100/E2000 take from the
    game plugin's `status`).
- **Machine ↔ game:** defined by each machine.  For E2100 it is
  `machines/e2100/e2100.h`.
  - The game exports `e2100_game_entry()`, which returns `struct
    e2100_game`: `reset`, `decrypt_from_game`/`encrypt_to_game` (the channel
    cipher), `message` (each game → ASIC message), `batch_done`, `build` (the
    ASIC's next batch, every 16 ms), and optionally `key` and `draw`.
    Register-table games set `reg_cmd`/`reg_build` instead,
    request/response games `exchange` (game bytes in, reply bytes out, one
    event per ring, no periodic batches).  `serial` lists device paths with
    a peripheral: the machine opens them as ports and hands each write to
    the plugin, whose answer the game reads back.
  - The machine supplies `batch_add` and the channel indices (`struct
    e2100_channel`).
  - Every game callback runs under the machine's lock.
- **Portability:** machines and games are plain C with no libc
  interposition, SDL or GL.  A future native-Windows core can reuse them
  unchanged.

**Game settings** come from the software lists (`hash/*.xml`, see Rom
sets): `run.sh` exports the machine, plugin, asset mask, variant and
description to the plugins as `IGS_MACHINE`, `IGS_GAME`, `IGS_ASSET_MASK`,
`IGS_VARIANT`, `IGS_NAME`.
## What each layer stands in for

| Game does | Handled by |
| --- | --- |
| `open("/dev/pccard0")` | machine: placeholder fd, starts the ASIC thread |
| ioctl `0x40046602` + `mmap` | machine: BAR0 = 512 KiB shared memory.  BAR1 = a core MMIO trap, because the mailbox at `0x100..0x11F` has separate read and write sides |
| ioctl `0x6600` / `0x6601` | machine: ASIC reset / notify (the boot handshake), then batches.  Cipher and messages go to the game plugin |
| ioctl `0x80046603` / `0x40046604` | machine: read / clear the event counter (one event per ASIC action) |
| ioctl `0x40106606` / `0x80106605` | machine: SRAM write / read, descriptor `{buf, bar, len, sram}` |
| ioctl `0xC0106610` / `0xC0106611` | machine: asset cipher, request `{encrypt, src desc*, dst desc*, key*}` |
| ioctl `0x4024660F`, `0x6607`–`0x660E` | machine: key and mode writes, ignored (the mask comes from the software list) |
| `open("/dev/mixer")` + OSS ioctls | core: an empty mixer (the games exit without one) |
| `settimeofday`, `/dev/rtc` | core: no-op / fails harmlessly |
| `readdir`, `__xstat`, `__lxstat`, `__fxstat` | core: backed by the 64-bit calls with the inode number truncated (see below) |
| `SDL_PollEvent` | core: key events go to the machine, then the game (SDL 1.2 `keysym.sym` is at event offset 8) |
| `SDL_SetVideoMode` | core: drops the fullscreen flag unless `IGS_FULLSCREEN` |
| `SDL_GL_SwapBuffers` | core: overlay (the game's `draw`), then the 60 fps cap, then the frame-rate log |
| `Mix_OpenAudio`, `Mix_Play*`, `Mix_Halt*`, `Mix_Volume` | core: buffer override and call logging |
| *(SDL linked into the game, E2000)* | |
| `XOpenDisplay` | core: clears the display's lock pointers (see below) |
| `XNextEvent` | core: key events (X keysym → SDL 1.2 symbol) to the machine |
| `dlsym(…, "glXSwapBuffers")` | core: hands out a hook: overlay, frame cap, swap |
| `glXSwapBuffers` called directly (Ogre's GL render system) | core: the same hook, interposed |
| `XGrabKeyboard`, `XGrabPointer`, `XWarpPointer`, `XAutoRepeatOff` (OIS) | core: no-ops unless `IGS_X11_GRAB=1` |
| `SDL_WM_SetCaption` | core: ignored while the loader's title is on; the core sets its own every second |
| `XStoreName`, `XSetWMName`, `X*SetWMProperties`, `XSetWMHints`, `XSetClassHint` | core: remembers the window as the game's top-level; the title goes on it through a connection of the core's own (`WM_NAME` and `_NET_WM_NAME`) |
| `read`/`write`, `tcsetattr`/`tcgetattr`/`tcflush` on a machine fd | core: to the machine (`read`/`write`, `ioctl` `TCSETS`/`TCGETS`/`TCFLSH`) |
| `/dev/ttyS0`, `/dev/ttyS1` (Hero of Robots) | machine `e2100`: serial ports; the game plugin answers each write (QR reader, card dispenser) |
| `libcasmcard.so` `Cas*` (PC/SC smart card, Hero of Robots) | preloaded `build/libcasmcard-hero.so` (SONAME `libcasmcard.so`): one reader, one card |
| `glXSwapInterval{SGI,MESA}` via `dlsym` / `glXGetProcAddress[ARB]` | core: no-op (Xwayland answers GLXBadContextTag) |
| `open("/dev/dsp")` + OSS ioctls + `write` | core: a pipe drained by a thread into PulseAudio (`libpulse-simple`) |
| `signal` / `sigaction(SIGSEGV)` | core: kept on the MMIO trap handler while traps exist; the game's handler becomes the fallback |
| `/dev/plx/Pci9030*`, `/dev/roio`, `/dev/pccard0` | machine `e2000` (`IGS_E2000.md`) |
| `/lib/libcasmcard.so` (linked by absolute path) | preloaded stub `build/libcasmcard.so` whose SONAME is that path (feature `preload`) |

**Details:**
- **MMIO traps.** The trapped instructions are 8/16/32-bit `mov` (`88 89 8A
  8B C6 C7`, with an optional `66` prefix) and `movzx` (`0F B6/B7`).  An
  unsupported instruction aborts and logs its bytes.
- **32-bit `readdir`/`stat`.** WSL's Windows-drive mounts report inode
  numbers around 2^50, so the games' 32-bit calls fail with `EOVERFLOW`.
  That broke Ocean Party's file index.
- **Overlay.** It is drawn with fixed-function GL with all state saved.  GL
  entry points come from `SDL_GL_GetProcAddress`, because the games export
  data symbols with GL names; from `glXGetProcAddressARB` when SDL has no
  GL (SDL linked in, or only used for audio as in Hero of Robots).

- **E2000: SDL inside the executable.**  The games link SDL 1.2 (X11/GLX
  video, OSS audio) and SDL_mixer statically, so no SDL symbol can be
  interposed.  The core works one level lower:
  - **Xlib locks.**  libX11 ≥ 1.8 enables its display locks for every
    client, and the game's built-in 2008 Xlib extension code deadlocks in
    the first `XMatchVisualInfo`.  The core clears `lock` and `lock_fns` in
    the `Display` (offsets 1220/1232 on i386), so Xlib runs single-threaded
    as it did on the cabinet.  Xlib prints a warning about it, which is
    expected.
  - **SDL's parachute.**  It re-installs the previous SIGSEGV handler with
    `signal()`, which drops `SA_SIGINFO`; the trap handler then crashed on
    the first BAR access.  `signal`/`sigaction` for SIGSEGV are interposed
    to keep it.
  - **GL entry points** for the overlay come from `glXGetProcAddressARB`
    when the game does not export `SDL_GL_GetProcAddress`.
  - **List features:** `video_driver` `x11`, `audio_driver` `dsp`,
    `preload` `libcasmcard.so`.

## Adding a game

1. **Work out the image** (in a scratch workspace if needed):
   - partitions (`IGS_ROFS_REVERSE_ENGINEERING.md`)
   - the ROFS key and profile
   - the asset mask (`IGS_SOUND_CRYPTO.md`)
   - how the cabinet boots it (`/etc/X11/IGS`, the kernel): where the
     partitions mount, where the executable is at run time.
2. **Add a `<software>` entry to `hash/<machine>.xml`** (id = the dump name
   without the machine prefix; the disk SHA-1; features; a `build` recipe),
   add the keys to `IGS_KEYS.md`, and build the set with
   `romtool/mkromset.py <id> <image>`.  Copy the hashes it prints into the
   entry's `<rom>` lines.
3. **Start with a copy of the closest game plugin** in
   `games/<game>/<game>.c`.
   - The channel cipher and message set come from the game binary's
     send/receive routines.
   - Compare with `IGS_E2100_PROTECTION.md`; Harpoon and Ocean already
     differ completely.
4. **`build.sh`, then `run.sh <id>`.**  Things to look for:
   - `unknown ioctl` in the log
   - an MMIO abort (an instruction the trap doesn't know)
   - `rx checksum mismatch` (wrong channel cipher)
   - a SYSTEM ERROR screen (the game rejected a batch)
   - garbled sounds or textures (wrong asset mask)

A new **machine** (E2000, E2200…) gets its own `machines/<name>/`, with its
own device emulation and game interface.  The core stays as it is.  Before
fixing that interface, bring up one game of the machine first, to see which
parts its games really share.

## Audio

Uncapped, the games render as fast as llvmpipe allows (about 75 fps) and keep
every CPU busy.  WSL's audio path then misses its deadlines:

- WSLg's sink gets 150–950 ms gaps, which sound like near-silence with short
  fragments.
- The game's own output, captured with SDL's `disk` driver, has no gaps.
- `paplay` alone is clean.
- A bare 32-bit SDL_mixer test gets the same gaps as soon as all CPUs are
  busy.

The 60 fps cap leaves about 3 cores idle and the gaps disappear.  Raising the
audio buffer to 4096 samples (`PULSE_LATENCY_MSEC=100`) made it *worse* (80%
silent) and could stall the game.  Measure changes by recording
`RDPSink.monitor` with `parec` (package `pulseaudio-utils`).

## GPU and Windows

- **GPU.** WSLg's GPU OpenGL (Mesa d3d12) needs Windows' `libd3d12.so`, which
  only exists as 64-bit.  32-bit games therefore get llvmpipe on the CPU,
  which is enough for both games so far.  If a heavier game needs the GPU,
  the plan is a GL bridge: a 32-bit `libGL` that forwards to a 64-bit helper
  process over shared memory.  A native Windows port needs the same piece.
- **X11.** On WSLg's Xwayland, Mesa's software GLX path answers the
  swap-interval request with `GLXBadContextTag` and the game dies.  Wayland
  with EGL avoids GLX entirely.
- **Windows native.** See the next section: a native `.exe` runs the SDL
  games (E2100) on the Windows GPU, with no WSL at run time.

## Windows

`win/` is a second core: **`igs-loader.exe`**, a 32-bit
Windows program that loads the game's i386 ELF binary itself and runs it
natively.  No WSL, VM or X server at run time; GL goes to the real Windows
driver, so the games get the GPU.

Status (2026-10-30, RTX 3060, Windows 11):

| Game | State |
| --- | --- |
| Harpoon Lagoon M107US | attract and play, picture, sound, keyboard; 60 fps |
| Harpoon Lagoon M001US-WA, M200US-WA | M001US-WA runs to attract from its `.7z` set (2026-10-30); M200US-WA links cleanly (`--check`) |
| Ocean Party md105cn | attract, coins, rounds, overlay; sound |
| Scratch m101cn | attract and coins at 60 fps; `libmikmod.so.2` missing (SDL_mixer's MOD support only) |
| Hero of Robots, Gold Captain | not yet (see below) |

### Usage

```powershell
.\play-win.ps1 harpoonlagoon_m107us
.\play-win.ps1 oceanparty_md105cn
.\build\win\igs-loader.exe --list
.\build\win\igs-loader.exe --verify scratch_m101cn
.\build\win\igs-loader.exe --check scratch_m101cn
```

The argument is a game id (Rom sets).  The exe finds its loader root (the
directory with `hash\` and `igs-loader.ini`) next to itself or two levels
up (`build\win\`), parses the software lists itself, finds the set on the
rompath and extracts an archive into the cache (`<cachepath>\windows\<id>`)
in-process (streaming 7z, about 70 MB of memory).  It sets the same
environment as `run.sh`: `IGS_MACHINE`/`IGS_GAME` (the DLLs next to the
exe), `IGS_ASSET_MASK`, `IGS_VARIANT`, `IGS_NAME` (the description) and the
`env` feature.
`--check` loads and links everything, prints the modules and every
unresolved symbol, and exits (status 1 if any are missing).  It never starts the game.

The variables from Usage above apply where they make sense: `IGS_SHOT`,
`IGS_SHOT_EVERY`, `IGS_SCRIPT`, `IGS_FPS_CAP`, `IGS_FPS`, `IGS_VERBOSE`,
`IGS_TITLE`, `IGS_FULLSCREEN`, `IGS_AUDIO_CHUNK`, `IGS_NULL_DEVS`, and the
game plugins' own.  Windows only:

| Variable | Effect |
| --- | --- |
| `IGS_AUDIO_LOG=1` | log audio callback counts and peak level every 5 s (the first non-silent buffer is always logged) |
| `IGS_BACKGROUND=1` | open the window without taking the keyboard focus (test runs) |
| `IGS_VSYNC=1` | also wait for vblank (default: the frame cap alone paces) |
| `IGS_MISSING_OK=1` | start even with unresolved symbols (they log and return 0 when called) |
| `IGS_LDDEBUG=1` / `2` | loader detail: modules, inits, dlopen/dlsym / every symbol binding |
| `IGS_ROMPATH`, `IGS_CACHE`, `IGS_HASHPATH` | as in WSL: override the `igs-loader.ini` settings |

The rom set is the cabinet's root fs: the exe takes the system libraries
the game was built against from the set's `/lib` and `/usr/lib`, and
absolute paths the game opens resolve inside the set.

### Build

In WSL (needs `i686-w64-mingw32-gcc-posix`, mingw-w64 13):

```bash
./win/build-win.sh
```

It produces `build/win/igs-loader.exe`, `e2100.dll` and the game plugin
DLLs.  Everything is linked statically, so no other DLLs are needed.  The
plugins build from their Linux sources unchanged:
`win/compat/igs_plugin.h` is force-included, and `win/compat/` provides
`<dlfcn.h>` (LoadLibrary) and `<sys/mman.h>` (anonymous VirtualAlloc).  It
also pins the plugins' condition-variable deadlines to `CLOCK_REALTIME`,
which is the clock winpthreads measures them against.
`win/elfdeps.py <id> [--set <dir>] [--all]` (WSL) follows `DT_NEEDED` offline the
way the exe does and lists the symbols no loaded ELF module defines and
`win/*.c` does not provide.

### How it works

- **What is ELF and what is native.** The game binary and its ordinary
  libraries are loaded as ELF, unchanged, from the game's `libs`, then
  `DATA/lib`, `DATA/libs` and `DATA`, then the set's `/lib` and
  `/usr/lib`.  That covers libstdc++ 6.0.10, libgcc_s 4.3.2,
  SDL_mixer, GLU, zlib, libmad, libjpeg, GLEW, zziplib, and
  libvorbisfile/libogg (dlopened by SDL_mixer).  The OS-boundary libraries
  are native (`win/*.c`):
  - libc, libm, libpthread, libdl;
  - SDL 1.2;
  - libGL/GLX;
  - empty stand-ins for libXxf86vm, libpci, ncurses and X11 helpers that no
    game here calls.
- **Loader (`elf.c`).**
  - Address space: the executable maps at its link address (main reserves
    `0x08000000` before anything else allocates).  Libraries map anywhere.
    All pages are RWX.
  - Symbol lookup follows ld.so order, with symbol versions.  The native
    table sits where the first system library is in the `DT_NEEDED` order.
  - Relocations: REL and RELA.  Fedora's prelinked libraries carry RELA.
    The executable is relocated last, so its `R_386_COPY` relocations (C++
    vtables, `stderr`) copy relocated data.
  - TLS: dynamic, through `___tls_get_addr`.
  - Init: `DT_INIT` and init arrays in dependency order, then the ELF
    `_start` with a kernel-style stack.
  - Library lookup: by `DT_SONAME` among regular files named like the
    library.  WSL symlinks are skipped.
  - `dl_iterate_phdr` serves libgcc_s's unwinder; `dlopen` and
    `SDL_LoadObject` load ELF at run time.
- **Stack protector.** The cabinet libraries read their canary at
  `%gs:0x14`, and nothing else uses `%gs`.  At load, those reads
  (`mov eax`, `mov reg`, `xor reg`, disp32 0x14) become `%fs:0x14`, which is
  `NT_TIB.ArbitraryUserPointer`, a per-thread word.  `__stack_chk_fail`
  logs once and returns.
- **Calls into ELF code** (thread entries, the audio callback, qsort
  comparators, init functions, RWops callbacks) go through `elf_call`, which
  aligns the stack to 16 bytes.  Threads also set x87 precision to 64 bits,
  as on Linux.
- **GL (`gl.c`).** Every GL entry point is a 16-byte stub into one shared
  thunk.  The thunk:
  1. saves the return address and `esp` in two TLS slots
     (`fs:[0xE10+4i]`);
  2. calls the stdcall function;
  3. restores `esp`, so no argument counts are needed.

  Targets come from opengl32's exports or, once a context exists,
  `wglGetProcAddress`.  Missing ones return 0.
  `glXGetProcAddressARB` answers with the same stubs.
- **SDL (`sdl.c`).**
  - Video: a Win32 window with a WGL context; GL attributes become the
    pixel format.
  - Keyboard: virtual keys become SDL 1.2 key symbols, as SDL events and to
    the machine plugin.
  - Audio: waveOut with the SDL 1.2 callback model; at least about 23 ms per
    buffer, 3 buffers.
  - The rest: RWops, WAV loading (PCM), `SDL_BuildAudioCVT`/`ConvertAudio`
    (format, channels, linear resampling), threads, mutexes.
  - Structures use the SDL 1.2 i386 Linux layouts.
- **libc (`libc*.c`).**
  - Layouts and tables: glibc's 32-bit layouts (`stat`/`stat64`, `dirent`,
    `struct tm` with `tm_gmtoff`), glibc's ctype tables, and the C locale
    as libstdc++'s "gnu" model reads it (`__newlocale` returns a
    `__locale_struct`).
  - Characters and numbers: 32-bit `wchar_t`; glibc's `random()`
    (`RAND_MAX` 2^31-1, the same sequence); mingw's C99 printf/scanf.
  - `FILE` is a wrapper of the CRT's.
  - Pthreads keep their Win32 object in the first word of each pthread
    object (0 = static initializer).  Both `pthread_cond_t` versions fit.
  - `setjmp`/`longjmp` are our own.
- **Paths.** Relative paths resolve in DATA, which is the cwd.  For absolute
  paths:
  - `/dev/*` goes to the machine plugin or the empty mixer;
  - `/mnt/<d>/` is drive `<d>`;
  - `/tmp` is `%TEMP%\igs-tmp`;
  - anything else is looked up in the set (the cabinet's root), then DATA.

  In an archive set's cache the symlinks are already copies (Rom sets).  In
  a folder set made in WSL, WSL symlinks (reparse tag `0xA000001D`) are
  followed by reading their target.  Either way `OceanParty` and `SWIN`,
  links into `*.bin/`, load.
  `getcwd` reports `/mnt/<d>/...`.
- **Core (`core_win.c`).** The same features as `core/core.c`:
  - the machine DLL's device files;
  - the empty OSS mixer, `IGS_NULL_DEVS`, no clock setting;
  - overlay, `IGS_SHOT`, `IGS_SCRIPT`, frame cap, window title.

  MMIO traps are `PAGE_NOACCESS` regions emulated in a vectored exception
  handler, using the same mov decoding as `core.c`, on `CONTEXT`
  registers.  A fault in ELF code is fatal; it logs the module+offset, the
  registers and a rough stack of ELF return addresses.  The game's own
  `signal()`/`sigaction` handlers are recorded, never called.

### Stubbed or faked

- **Processes:** `fork`, `exec*`, `system`, `popen` and `waitpid` fail and
  log.  Ocean Party calls `system` and `socket` and carries on.
- **Network:** sockets fail.
- **Signals:** never delivered.
- **Threads and the unwinder:** `pthread_cancel` does nothing.  Futex
  `syscall`s sleep 1 ms (libstdc++ static-init guards).
- **Memory:** `mmap` of a file is a private copy, so `MAP_SHARED` writes are
  not written back.
- **Locale:** the C locale only.
- **Video:** `IGS_FULLSCREEN` switches the display mode with
  `ChangeDisplaySettings` (untested).
- **Audio:** WAV loading handles PCM only.  `/dev/dsp` (E2000) is not
  implemented.

### Not yet: Hero of Robots, Gold Captain

- **Hero of Robots** (Ogre, OIS, X11).
  - It needs a native X11/GLX layer: window, `glXChooseVisual` /
    `glXCreateContext` / `glXMakeCurrent` / `glXSwapBuffers` on WGL, and
    keyboard events as `XNextEvent` key events for OIS.
  - Ogre and its plugins load as ELF through `dlopen`, which already works.
  - The loader is `--large-address-aware` for its memory.
  - Its `libcasmcard.so` preload would become a native module or an
    `IGS_PRELOAD` of the plugin.
- **Gold Captain** (SDL and X11 linked in statically, OSS `/dev/dsp`).
  - It needs the same X11/GLX layer plus `/dev/dsp` on waveOut.  The OSS
    ioctl handling of `core.c` carries over.
  - The `XOpenDisplay` lock workaround and the X11 grab no-ops become
    unnecessary.
- Both are reached by adding native symbol tables (`win/x11.c`).  The loader
  and the rest of the core do not need to change.
