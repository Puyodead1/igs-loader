# Scratch m101cn (IGS E2100)

A four-player scratch-card medal game, 2012, Simplified Chinese.  Each
player picks a ticket from one of six racks (bets 1, 5, 10, 20, 30 or 50),
and the game scratches it with the scratch dial.  The scenes are Flash
movies (`main/raw/*.swf.ifs`) drawn by a GL renderer built into the game.

**Status (2026-10-30): playable in the loader** (`play.ps1 scratch_m101cn`, about
20 fps), with one keyboard player: coins, credit, taking a ticket,
scratching, results.  The ASIC's side is re-implemented, so **the prizes
are made up**: a third of the tickets win 1–5× the bet.  The real odds tables
(ASIC words 43/44) are not known.

## Playing

```powershell
.\play.ps1 scratch_m101cn
```

| Key | Cabinet (player 1) |
| --- | --- |
| `5` | coin (+100 credit; `SCRATCH_COIN` changes it) |
| ↑ ↓ ← → | move between the racks and tickets |
| Ctrl, Space | red button: take a ticket, confirm |
| Alt | yellow button |
| `Z` / `X` | scratch dial left / right |
| F2 | Test (operator menu; player 1's arrows navigate, yellow exits) |

Players 2–4 have input bits but no keys yet.

## The disk

The image `e2100_scratch_m101cn` is 8,011,422,720 bytes (CHD data SHA-1
`22f0c8a9a6c1bce7fae7febc3cb9f84730164e8a`).  In the rom set:

| Cabinet path | What |
| --- | --- |
| `/` | p2 `igs_rofs` root filesystem (profile `scratch`); p1 holds `Scratch` (LILO) and `Scratch-OS` |
| `/Scratch/` | p3 SquashFS, the game's working directory: `main/raw/` Flash scenes, `sound/ogg/`, `lib/` (SDL, GLEW, zzip), `rw_data/` (p4 mount point); `SWIN` and `asic36.bin` link to `../Scratch.bin/` |
| `/Scratch.bin/swin` | **the decrypted game binary** (the builder decrypts it as the cabinet does at boot) |

| Partition | Start LBA | Bytes | Content |
| --- | ---: | ---: | --- |
| p1 | 63 | 12,354,048 | boot |
| p2 | 24,192 | 180,633,600 | `igs_rofs` root |
| p4 | 376,992 | 30,449,664 | `rw_data` |
| p3 | 436,464 | 1,680,408,576 | SquashFS game data |

**The executable is encrypted.**  On p2, `/Scratch/swin` is FPGA-cipher
data.  The kernel runs `/bin/unrar /Scratch/swin /Scratch.bin/swin
Hello-ScratchWin` at boot; `unrar` is a 5 KB IGS tool that hands the key
string to the FPGA and decrypts through it.  The cipher is the usual one
(`IGS_SOUND_CRYPTO.md`) with mask `e4be1b4e7f081654fa6b81698b039a06`,
recovered from the ELF header (`IGS_KEYS.md`).  The game's own assets are
plain.

## Start-up on the cabinet

`/etc/X11/IGS` copies `asic36.bin` to `/Scratch.bin/`, mounts p3 on
`/Scratch` and p4 on `/Scratch/rw_data`, starts Xorg, then loops
`cd /Scratch; ./SWIN` with `LD_LIBRARY_PATH=/Scratch/lib/:...`.

## Protection: the register-table transport

The boot handshake is Harpoon's (the loader's HLE works unchanged).  The
upload page key would be `pccard0 Open OK!`, and `asic36.bin` is read but
not used.  The runtime transport is different, and older: no batches, no
cipher.

- **Each frame** the game reads BAR1 header registers 0..63 (the ASIC's
  reply), writes 0 to all of them, writes its own table, and rings
  (`0x6601`).  The ASIC answers with exactly one event.  Code:
  `sub_80506E0` (exchange), `sub_804E750` (checksums), `sub_804F7B0`
  (dispatch), `sub_80501A0` / `sub_804F710` (game side).
- **Table:** word 0 = bitmask of words 1..25 present, word 32 = of 33..52;
  word 26 / 53 = the low byte of the sum of words 1..25 / 33..52; word 31 =
  bitmask of words whose value is a BAR0 byte offset of a data block.
- **Watchdog:** with no exchange for about 3–4.5 s the game prints `PSCR WDT
  DeadLock !!!` and stops.

### ASIC → game

| Word | Data | Meaning |
| --- | --- | --- |
| 1 | 24 dw | assert text: the game prints it and hangs |
| 2 | 10 dw | inputs: dword k's low byte = bits 8k..8k+7 |
| 3 | 19 dw | version: "I:"/"O:" text, fuses, FPGA version (answers 35) |
| 5 | 8 dw | `[4+p]` credit of player p |
| 7 | 28 dw | settings: 11 items, 8 password digits, 9 IR codes (answers 36) |
| 8 | 36 dw | errors, one-hot: 4 global, 8 per player |
| 11 | 8 dw | `[p]` payout, `[4+p]` tickets |
| 15 | 220 dw | records; `[216+p]` player state (0 idle) |
| 17 | 16 dw | **deal:** `[p]` flag, `[4+p]` prize, `[8+p]` bet view, `[12+p]` ticket id |
| 18 | 5 dw | jackpot event |
| 34 | value | RAM-clear confirm (answers 33) |
| 39, 43, 44, 49–52 | blocks | bookkeeping, win types, win odds, player records |
| 48 | 64 B | recover data (answers 47) |

### Game → ASIC

| Word | Meaning |
| --- | --- |
| 4 | clock `[mode, YYYYMMDD, HHMMSS]`, every exchange |
| 16 | cursor, every exchange: `[p]` = 100 · ticket kind + ticket index, `[4+p]` view level, `[8+p]` a flag |
| 21 | ticket results: `flag[4]`, `win[4]`, `bonus[4]`, `?[4]` |
| 12, 20, 19, 22–25, 9, 13 | result report, minor wins, records, lamps/meters, setup |
| 6 | settings save (then 36 to read them back) |
| 33, 35, 36, 38, 40, 41, 42, 47 | requests; the game repeats each until it is answered |

Ticket kinds (the hundreds of the cursor): 1, 6, 11/12, 21/22, 31–33,
51–53, for bets 1, 5, 10, 20, 30, 50.

**Lamps:** an RS232 lamp board on `/dev/ttyS0`.  The loader gives the game
a null port (`IGS_NULL_DEVS`), whose reads fail with `EAGAIN` as the
game's read loop expects.

## The loader's controller

The plugin is `games/scratch/scratch.c` on the machine's
register-table transport (`IGS_LOADER.md`).
- **Every reply:** inputs (word 2) and credits (word 5).
- **On request:** version, settings, records, RAM-clear confirm, recover
  data, and zeroed bookkeeping.
- **Coin:** adds `SCRATCH_COIN` credit.
- **Red pressed:** if no ticket is in play and the credit covers the bet of
  the ticket under the cursor, it takes the bet and deals that ticket
  (word 17).
- **Result (word 21):** the win plus bonus goes back to the credit.

## Display

- The game draws at 1920×1080.
- Its fragment shader starts with `precision lowp float;`, which plain GLSL
  1.10 rejects.  The software list's `env` feature sets Mesa's `force_glsl_version=130`.
- About 20 fps on llvmpipe.

## Open

- The real odds and prize tables, the bet view field, the jackpot (word
  18) and tickets out (word 11).
- Keys for players 2–4.
- Frame rate (20 fps; the cabinet runs at 60).
