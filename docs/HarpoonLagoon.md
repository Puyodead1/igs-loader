# Harpoon Lagoon (IGS E2100)

A four-station fishing/harpoon redemption game on the E2100 platform (Linux
2.6.32.8 on x86, NVIDIA GPU, IGS FPGA card with a protection ASIC).
Internal names: `Fish4P`, game binary `fish`.

Status: playable through the loader (`IGS_LOADER.md`, plugin
`games/harpoonlagoon`, about 45–60 fps in WSL, 60 fps natively on Windows).

## Versions

Three dumps exist.  They share the kernel (`Fish4P-OS`), the ROFS key, the
channel keys and the asset mask.  On the data partition only `asic36.bin`
differs, plus `action/player.rom` between M107 and the WA builds.  The
version string is in `fish`.

| Version | Image | Id (`hash/e2100.xml`) | Differences |
| --- | --- | --- | --- |
| M107US | `e2100_harpoonlagoon_m107us.img` | `harpoonlagoon_m107us` | operator menu has a language page (English, Spanish, Russian, Turkish) and "Maximum Bonus Ticket" |
| M001US-WA | `e2100_harpoonlagoon_m001uswa.img` | `harpoonlagoon_m001uswa` (clone) | adds the jellyfish jackpot (three `player.rom` animations) with "Number of Jellyfish" / "Number of Bonus" settings; no language page |
| M200US-WA | `e2100_harpoonlagoon_m200uswa.img` | `harpoonlagoon_m200uswa` (clone) | as M001US-WA, but those two settings are gone |

"WA" is most likely the Washington-state build.  Washington allows only
skill-based redemption games, which would explain the cut-down payout
settings; this is a guess from the names, not confirmed.

CHD data SHA-1s (both matched after `chdman extracthd`):
- m001uswa: `ab44a4f7de3a17b955751bca7f7e761778e798e0`
- m200uswa: `ad851e721abcad170dde7d36e66d77431d239629`

**The WA builds' operator settings (message 10) use another layout.**  The
block is at `0x8406BA0`, and word 17 is the language (0–3).  The language
also picks the cell bank of the pounds-to-tickets panel (`sub_806C010`:
`lang·8 + …`), so M107's value 4 stops the game at start-up with
`zlaction_iProcess() 32 over BasePicIndex:32 ActionNo:24`.  Their
software-list entries set the feature `variant` to `wa`, and the plugin then sends language 0.

## The disk

All three images are 1,903,878,144 bytes (the M107US raw image's SHA-256 is
`B7EB2B30…D1FE`):

| Partition | Contents |
| --- | --- |
| p1 | FAT boot: the `Fish4P` loader and the `Fish4P-OS` kernel |
| p2 | `igs_rofs` root filesystem (`IGS_ROFS_REVERSE_ENGINEERING.md`), with the game binary at `/Fish.bin/fish` |
| p3 | SquashFS game data, mounted at `/Fish`: `asic36.bin`, `sound/`, `action/`, `libs/`, fonts, and `FH -> ../Fish.bin/fish` |
| p4 | ext2 `rw_data` (unused by the loader) |

The rom sets hold p2 at `/` and p3 at `/Fish` (`igs-mkromset`).

## Formats and protection

- Root filesystem: `IGS_ROFS_REVERSE_ENGINEERING.md`
- Asset cipher (sound, action): `IGS_SOUND_CRYPTO.md`
- FPGA/ASIC protocol, button and credit messages, operator settings:
  `IGS_E2100_PROTECTION.md`

## Controls

Keys go to the game window.

| Station | Up/Down/Right/Left | Fire | Bet | Coin |
| --- | --- | --- | --- | --- |
| 1 | arrow keys | Ctrl | Alt | 5 |
| 2 | R / F / G / D | A | S | 6 |
| 3 | I / K / L / J | B | V | 7 |
| 4 | keypad 8 / 2 / 6 / 4 | keypad 0 | keypad . | 8 |

F2 is the operator Test button.  Station `p` uses button bits `8p+0..5`
(up, down, right, left, bet, fire), and Test is bit 78.

## Operator menu

Press **F2** to enter it.  Menu input is read by `sub_807FF50(code)`.  Each
code accepts either a cabinet button or an SDL key:

- **Cabinet buttons:** station 1's keys (through the ASIC button messages).
- **SDL keys:** read from the game's own window events.

| Code | Action | Cabinet (station 1) | SDL key |
| --- | --- | --- | --- |
| 0 | up | Up | ↑ |
| 1 | down | Down | ↓ |
| 2 | left | Left | ← |
| 3 | right | Right | → |
| 4 | select / confirm | Fire (Ctrl) | Enter |
| 5 | special, probably back/exit (not traced) | — | Right Shift |

Code 5 sets `dword_845A2F4 = 0` (or `dword_845A2F8 = 1`) and
`byte_845A2E0 = 1`.

- **Simple pages** (`sub_80802A0`): select sets `dword_845A2E4 = 2` and plays
  sound 33.
- **10-digit password page** (`sub_8082670`):
  - Up/Left increment the current digit and Down/Right decrement it, with
    wraparound.
  - Select confirms the digit and moves to the next.
  - After 10 digits the entry is compared with `dword_8406698` /
    `dword_84066C0`, which come from ASIC message 10.  The HLE sends zeros
    there, so **ten presses of Select with every digit at 0** pass.
  - A wrong code flashes an error and returns after about 4.5 s.
- Settings edits are sent back as message 9.  The HLE logs them but does not
  store them.

Not yet mapped: the full page list and how each page exits (likely
`dword_845A2D8` / `dword_845A2E4` together with code 5).

## Useful game addresses (`IGS/fish`)

| Address | What |
| --- | --- |
| `0x8067F10` | protection start-up |
| `0x80A2998` | `FPGA_AES_iEncrypt` (asset cipher via `/dev/pccard0`) |
| `0x8068320`, `0x806B610`, `0x8064C50` | asset / sound loading |
| `0x8063620`, `0x8062AD0`, `0x805E990` | ACT_ZL3D loading and step interpreter |
| `0x8054ADC` | `SDL_SetVideoMode(1280, 720, 24, FULLSCREEN\|OPENGL)` |
| `0x8054D80` | SDL key state query |
| `0x8069AA0` | cabinet button "pressed" |
| `0x807FF50` | operator-menu input |
| `0x8406620` | message-10 operator settings block |
| `0x8405A28` | message-55 word 10 (zero → SIGFPE at `0x806DCB8`) |
