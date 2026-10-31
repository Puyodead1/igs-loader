# Ocean Party md105cn (IGS E2100)

A fish game.  Its firmware upload key says "BigFish6P", probably six
players; not confirmed.  The board and OS are the same as Harpoon Lagoon's;
everything above the kernel driver is new.

Status (2026-10-29): **playable in the loader** (`play.ps1 oceanparty_md105cn`,
about 50 fps), with one keyboard player.  The ASIC's game logic is
re-implemented in the shim, so the odds and rules are an approximation.

## How to play (loader)

Ocean Party is a betting game.  Each round has three parts:

1. **Betting:** fish swim while bets are open, 20 s by default.
2. **Reveal:** one fish is revealed with its multiplier.
3. **Win:** bets on that fish pay bet × multiplier.

The last 10 results run around the screen edge.

| Key | Action |
| --- | --- |
| `5` | insert a coin (+10 credits) |
| `Q` `W` `E` `R` | bet 1 on Nemo / Butterfly / Snail / Angler (×5 each) |
| `T` `Y` | bet 1 on Turtle / Nautilus (×10) |
| `U` | bet 1 on Lobster (×20) |
| `I` | bet 1 on Crab (×40) |
| `X` | take back all bets (during betting) |
| `Esc` | quit (the game's own key) |

**The player panel** at the bottom shows credit, last win, the betting timer
or result, and the bets per fish.  It stands in for the cabinet's per-seat
displays, which belong to the ASIC.  The shim draws it with GL just before
each frame is shown.

**Odds:**
- A fish's chance is proportional to 1/multiplier, so every bet returns about
  93% on average.
- The multipliers are in `odds[]` in `games/oceanparty/oceanparty.c`.
- The real cabinet's odds live in the ASIC firmware, which is unavailable.

**Tunables** (environment):

| Variable | Default | Meaning |
| --- | --- | --- |
| `OCEAN_BET_SECONDS` | 20 | length of the betting period |
| `OCEAN_COIN` | 10 | credits per coin |
| `OCEAN_CREDITS` | 0 | starting credits |

The window is 1920×1080, so the title bar pushes its edges off a 1080p
screen.  `IGS_FULLSCREEN=1` avoids that.

## The disk

The image `e2100_oceanparty_md105cn`, from CHD with `chdman extracthd`:

- logical size 1,900,039,680 bytes
- CHD data SHA-1 `ee56bb261533c389619ed8d3b49d5e65b3471683`
- image SHA-256 `DB6BAB6E9DF8995CCFC2C0325C4CBFF29E499B5D203ED0E84957CD039DAF5A36`

| Partition | Contents |
| --- | --- |
| p1 | boot (ext3 despite MBR type `0x01`): `OceanParty` loader, `OceanParty-OS` kernel, `boot.b` |
| p2 | `igs_rofs` root filesystem (profile `oceanparty`), with the game at `/OceanParty.bin/OceanParty` |
| p3 | SquashFS, mounted at `/OceanParty`: `sound/` (85 WAVs), `action/` (26 `ACT_ZL3D` banks), `system/` (fonts), `asic36.bin`, and `OceanParty -> ../OceanParty.bin/OceanParty` |
| p4 | ext3, empty |

## Disk layout

| Partition | Start LBA | Bytes | Content |
| --- | ---: | ---: | --- |
| p1 | 63 | 12,354,048 | ext3 boot |
| p2 | 24,192 | 180,633,600 | `igs_rofs` root |
| p4 | 376,992 | 64,512,000 | ext3, `/OceanParty/ScreenCutOff` |
| p3 | 502,992 | 1,646,346,240 | SquashFS game data |

## Start-up on the cabinet

`/etc/X11/IGS`:

1. Mounts p3 on `/OceanParty` and p4 on `/OceanParty/ScreenCutOff`.
2. Starts Xorg with the NVIDIA driver.
3. Loops `cd /OceanParty; ./OceanParty`.

## Differences from Harpoon Lagoon

- **Keys and ciphers:** a different ROFS key and layout, asset-cipher mask,
  firmware key and runtime-channel cipher (RC4).  See `IGS_KEYS.md`,
  `IGS_ROFS_REVERSE_ENGINEERING.md` and `IGS_E2100_PROTECTION.md`.
- **Runtime messages:** a different set (types 1–10, with type 4 a large
  sub-switch).
- **Display:** 1920×1080, where Harpoon uses 1280×720.
- **File lookup:** at start-up the game scans its working directory
  recursively and registers every subdirectory as a search path
  (`sub_8158AF0`).  Fonts are found in `system/` that way.
  - The scan uses 32-bit `readdir`/`stat`, which fail on WSL's Windows-drive
    mounts (inode numbers above 2^32).  The loader shim provides 64-bit-backed
    versions.
- **Libraries:** the same set as Harpoon except no `libpci`.

## Key addresses (`OceanParty`)

| Address | What |
| --- | --- |
| `0x8068B80` | protection start-up (open `/dev/pccard0`, handshake, channel keys) |
| `0x8067130` | send batch (game → ASIC) |
| `0x8068910` | receive batch (ASIC → game), SYSTEM ERROR 02 on checksum mismatch |
| `0x8068AB0` | per-frame channel service |
| `0x8067610` | builds game → ASIC messages 6, 8, 9, 15 |
| `0x8067C30` | dispatches ASIC → game messages 1–10 |
| `0x806AAF0` / `0x806ABD0` | RC4 key schedule / crypt |
| `0x8067360` / `0x8067500` | SYSTEM ERROR 02 / 01 screens |
| `0x8055BE0` | display init, font load (`ChineseFont16.fon`) |
| `0x80A64E0` | file open through the search-path index |
| `0x8158AF0` | recursive search-path scan |
| `0x8055440` | SDL key state query |

## What the ASIC controls

Ocean Party is a ring betting game: a wheel of slots around the edge, fish
in the middle, and four player sides.  Unlike Harpoon Lagoon, the PC is
mostly a display; the ASIC runs the game.

**Game flow** (`sub_806C2B0` main loop on `dword_83E7918`):

| Type-3 mode (word 0 bits 0–5) | Game state |
| --- | --- |
| 0 | idle: background only (what empty batches give) |
| 1 | swim loop (`sub_806C0A0`): fish and music.  The loop checks the mode every 60 frames: 2 → transition, 4 → settings, 0 → idle |
| 2 | transition (`sub_806B3D0`): scene from type 7, byte 0 (`dword_83E4C84`), plus flag bits |
| 3 | round (`sub_80756B0`), then back to swim |
| 4 | settings: type-4 sub-messages; the sub-id is readable via `sub_8066220` |

**Other type-3 fields:**
- **word 0 bit 6:** music on.  Off mutes the music with `Mix_SetPanning(0,0)`
  (`sub_806CD80`).
- **word 0 bit 7:** sound effects on (`sub_806CE40`).
- **word 0 bits 8–23:** speed, used as frame scale `(v−1)/60 + 1`.  1 is
  normal.
- **word 0 bits 24–27 (`dword_83E5BF4`):** values 1–4 open a special screen
  loop (`sub_8097760`) that shows the per-seat fields from words 3–10.
- **word 1:** 8 nibbles (`dword_83E5C60[]`), per-seat UI values
  (`sub_806FE39`, `sub_8071389`…).
- **word 11:** next RX index and key.

**Type 2** is a config block of at least 26 words:

| Word | Meaning |
| --- | --- |
| 19+ | settings passed to `sub_8069270` |
| 23 | flag (`sub_8097C90`) |
| 24 | scene choice |
| 25 | shows the "confirm" screen at boot (wait for F12) |

**Start-up:**
- The game sends type 15 with the cabinet's **eth0 MAC address**, apparently
  so the ASIC can bind to the board.
- It waits up to 5 s for ASIC data at several points (`sub_8067050`).

**Developer keys** in the frame loop (`sub_806BB20`):

| Key | Effect |
| --- | --- |
| Esc | quit |
| `0` | pause; `9` resumes |
| `/` | toggle the info overlay |
| F12 | screenshot or confirm |

**Cabinet buttons** never reach the PC directly.  The game forwards its SDL
key bitmap to the ASIC as type 9; the real panel is read by the ASIC.

**Firmware:** emulating the real one is not practical yet.  Both games'
`asic36.bin` images have no repeated 16-byte blocks, so they aren't
encrypted with the simple per-block FPGA cipher, and the ASIC's CPU is
unknown.

### Results, history and the round handshake

**Type 7** (one word) carries the result:

| Field | Meaning | Global |
| --- | --- | --- |
| byte 0 | winning fish index | `dword_83E4C84` |
| byte 1 | multiplier, shown as "×N" | `dword_83E4C88` |
| bit 16 | special outcome 9 | `dword_83E4C8C` |
| bits 20–23 | special outcome 8 | `dword_83E4C90` |
| bits 24+ | special outcome 10 | `dword_83E4C94` |

The specials are the sharks.  They are used only when type-2 word 24 is set
(`sub_806BF00`, `sub_806B3D0`).

**Fish index → model**, from the game's model table:

| Index | Model |
| --- | --- |
| 0 | Nemo |
| 1 | Butterfly |
| 2 | SeaSnail |
| 3 | AnglerFish |
| 4 | SeaTurtle |
| 5 | Nautilus |
| 6 | Lobbster |
| 7 | Crab |
| 8–10 | Shark (specials) |

The paytable bars show them right to left.

**History:**
- Type 3 word 1 holds 8 nibbles (`dword_83E5C60[]`); word 2 bits 0–7 hold 2
  more.  Together they are the last 10 results, drawn on all four sides
  (`sub_806FD70`).  Values 0–7 are fish, 8–10 specials, above 10 blank.
- Word 2 bits 8–15 (`dword_83E5DA0`) are a level shown on the four sides,
  clamped to 1–10.

**Game state in the heartbeat:** type 8 word 0 bits 0–2 carry the game's
state.

| Value | State |
| --- | --- |
| 1 | swim |
| 2 | transition |
| 3 | round |
| 4 | settings |

**A round, from the ASIC's side:**
1. Mode 2, with the result in type 7.
2. The game runs the transition (state 2), then the round animation
   (state 3).
3. Once state 3 is seen, mode goes back to 1.  Otherwise the swim loop, which
   checks the mode every 60 frames, would start another transition.
4. The game returns to state 1; settle the bets.

**Seats:**
- 6 or 8, depending on type-2 word 23 (`sub_8097C90`).
- **Credits** come from type-4 sub 1, words 2–9 (`dword_83E5594`).  They are
  only shown in the operator screen (`sub_8087770`).
- **Payout and error state** per seat comes from type 3 words 3–10:
  `dword_83E5C00` = state 0–5, `dword_83E5C20` = amount, `dword_83E5C40` =
  message.  It is drawn as an overlay by `sub_8097760`.
- **Bets** are not shown on the main screen at all: on the cabinet they are
  on the per-seat displays.

### The loader's controller

The plugin is `games/oceanparty/oceanparty.c`
(`IGS_LOADER.md`).  Its `step()` runs once per ASIC batch, from `build`:

| Phase | What happens |
| --- | --- |
| WAIT | until the game first reports state 1 (after loading) |
| BET | mode 1 for the betting period; keys place bets |
| SHOW | mode 2 + type 7 → wait for state 3 → mode 1 → wait for state 1 → settle, push the history, back to BET |

Every batch carries type 3, with mode, music and effects enabled, speed 1,
the history, and the unchanged RX index and key, plus type 7.

## Next steps

- **Several players:** the HLE could give each seat its own keys and panel.
- **The sharks:** set type-2 word 24 and send specials 8–10 now and then.
- **Operator menu:** mode 4 and the type-4 sub-messages are mapped only as
  far as the credit screen.
