# Fish Hunter 4 (IGS E2100)

A shooting fish game for four stations around one screen: each station
turns its cannon, picks a bet of 1–5 (1–8 on the 4P build) and fires;
caught fish pay their value × the bet.  Chance games (a wheel, a
"small game", the star bomb) and a four-seat bonus game come up now and
then.  The IGS036 decides every catch and holds the credits; the PC draws.

Status: playable through the loader on both front ends (plugin
`games/fishhunter4`).  The chip's firmware is not dumped: its game logic is
rebuilt in the plugin (below), so odds and payouts are not the cabinet's.

## Versions

Two builds share the protocol: the CN build `FishHunter` (kernel `Fish-OS`, built 28 Oct 2010) and the four-player `FishHunter4P`
(`Fish4P-OS`, the kernel Harpoon Lagoon, its US version, also uses).

| Version | Id (`hash/e2100.xml`) | Kernel, ROFS profile | Variant | Executable |
| --- | --- | --- | --- | --- |
| M107CN | `fishhunter4_m107cn` (parent) | `Fish-OS`, `fish` | `cnrc4` | 2,534,353 bytes, symbols kept |
| M101CN | `fishhunter4_m101cn` | `Fish-OS`, `fish` | `cn` | 2,502,372 |
| M102CN | `fishhunter4_m102cn` | `Fish-OS`, `fish` | `cn` | 2,509,833, symbols and debug info kept |
| M103CN | `fishhunter4_m103cn` | `Fish-OS`, `fish` | `cn` | 2,517,322 |
| M104CN | `fishhunter4_m104cn` | `Fish-OS`, `fish` | `cn` | 2,519,774 |
| M105CN | `fishhunter4_m105cn` | `Fish-OS`, `fish` | `cn` | 2,532,492 |
| M400CN | `fishhunter4_m400cn` | `Fish4P-OS` #38, `fish4p_m400` | `4p` | 2,682,280 |
| M401CN | `fishhunter4_m401cn` | `Fish4P-OS` #3 (Harpoon's), `fish4p` | `4p` | 2,682,280 |
| M402CN | `fishhunter4_m402cn` | `Fish4P-OS` #3, `fish4p` | `4p` | 2,683,080 |

The variants follow the protocol's three builds: `cn` (M101–M105CN: no
channel cipher, 19-word settings), `cnrc4` (M107CN: the CN loader with the
RC4 channel and the 4P build's 37-word settings and records) and `4p`.

**M101CN's dump starts 511 bytes into the disk**: its first byte is the
MBR's last (`0xAA`), the partition table is gone and the image is padded
at the end.  The partitions are where the other M10x dumps have them,
which the data confirms (the ROFS text one byte into sector 64259, the
SquashFS magic one byte into sector 530144).  Its entry gives the layout:
features `dump_skip` (511) and `partitions` (`start+count` sectors).

## The disk

| Partition | M10x (LBA, sectors) | M40x | Contents |
| --- | --- | --- | --- |
| p1 | 63 + 64,197 | 63 + 48,321 | ext2 boot (MBR type `0x01`): LILO map `Fish` / `Fish4P`, `boot.b`, the kernel |
| p2 | 64,260 + 385,560 | 48,384 + 352,800 | `igs_rofs` root, `/Fish.bin/fish` and `/Fish.bin/asic36.bin` |
| p3 | 530,145 + 3,180,870 | 460,656 + 3,257,856 | SquashFS 4, mounted at `/Fish`: `action/`, `sound/`, `system/`, `asic36.bin`, `fish -> ../Fish.bin/fish` (M40x: `FH`) |
| p4 | 449,820 + 80,325 | 401,184 + 59,472 | ext2, empty: `/Fish/ScreenCutOff` (the game writes `password.dat`, `setting.dat` there) |

`/etc/X11/IGS` mounts p3 and p4, starts Xorg and loops `cd /Fish; ./fish`
(M40x `./FH`).  The sets hold p2 at `/` and p3 at `/Fish`.

**`Fish-OS` has no AES**: its `igs_rofs` is a cramfs-like plaintext stream
(profile `fish`, `IGS_ROFS_REVERSE_ENGINEERING.md`).  M400CN's kernel is an
earlier `Fish4P-OS` build with Harpoon's key and constants and an earlier
record layout (profile `fish4p_m400`).  Keys: `IGS_KEYS.md`.

## Controls

Keys go to the game window.  Station `p` uses input bits `8p+0..5` (up,
down, right, left, bet, fire), Test is bit 78, as on Harpoon Lagoon.

| Station | Turn left / right | Up / Down | Fire | Bet | Coin | Key-in | Payout |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | ← / → | ↑ / ↓ | Ctrl | Alt | 5 | F5 | F9 |
| 2 | D / G | R / F | A | S | 6 | F6 | F10 |
| 3 | J / L | I / K | B | V | 7 | F7 | F11 |
| 4 | keypad 4 / 6 | keypad 8 / 2 | keypad 0 | keypad . | 8 | F8 | F12 |

- **Coin:** credits by the "coin rate" setting (1, 2, 5, 10, 20, 50).
- **Key-in:** the operator credit key, the key-in rate's credits.
- **Payout:** all credits out as tickets (payout mode 3, the default; the
  "tickets" setting is credits per ticket, or tickets per credit above
  100) or coins (mode 1, the key-out rate); the station's payout panel
  counts down, 10 per second.
- **F2** opens the operator menu (book, test, system) between games; its
  keys are station 1's (Up/Down move, Fire enters, Bet leaves).

## Tunables

| Variable | Default | Meaning |
| --- | --- | --- |
| `FISH_RTP` | 0.95 | expected return of a shot at odds level 5; each level up takes 0.02 off |
| `FISH_CHANCE` | 300 | one shot in n starts a chance game; 0 = never |
| `FISH_CHANCE_KIND` | 0 | [Testing] 0 random, 1 tiny game, 3 small game, 4 big bomb, 5 bonus game |
| `FISH_CREDITS` | 0 | starting credits per station |

## Protection and runtime channel

The "e2k1" ShareRam protocol (`shareram_e2k1/pcshareram.c` in M102CN's
symbols): batches of messages, `header = type | length << 7`.

- **Framing:** BAR1 register 0 holds `count | payload words << 7` in both
  directions; BAR0 holds `[payload, headers, sum of both]`.  No running
  checksum, no index rotation: the game sends a batch and rings, the chip
  answers once (`_vSendTo36` `0x8063AA0`, `_uipRecvFrom36` `0x8063B60`).
  The machine's `E2100_LAYOUT_TRAILING`.
- **Cipher:** none on M101–M105CN (the RC4 code is linked but never
  called); M107CN and the 4P build run RC4 per direction over the whole body, keys
  `0x82675251` (chip → game) and `0x80678267`, set once at start-up.
  Harpoon Lagoon, the later US build, XORs words with the same two keys.
- **Boot:** the CN builds' P2A loader (`P2A_LoaderAdv_iLoadData`
  `0x80860E0`) compares the DONE word with the checksum of its upload, so
  the machine sends 0 there (`E2100_HS_NOSUM`); the 4P build uses
  Harpoon's loader (upload key `Open pccard0 !!!`).

| Game → chip | Words | Meaning |
| --- | --- | --- |
| 3 | 1 | version, please (→ 4) |
| 5 | 9 (4P, M107 11) | every batch: game state, bet per seat since the last batch (`bet`, `bet + 100` for the idle auto-shot, 99 for the star bomb), date `yyyymmdd`, chance-max flag |
| 7 | 1 | settings, please (→ 10) |
| 8, 11, 32 | 1 | records, please (→ 12) |
| 9 | 19 (4P, M107 37) | new settings (`SettingPC`) |
| 13 | 7 | catch request: `player << 16 \| weapon << 8 \| bet (+100 boosted)`, 12 bytes of fish per kind the game's own damage count says are due, 12 bytes of fish hit per kind (→ 14) |
| 25 | 7 | the same for a bomb (no hints) |
| 16 | 16 | lamps and counters |
| 19 | 1 | reset all (→ 20) |
| 24 | 2 | the swell fish was hit `[player, level]` |
| 28 | 1 | chance game over: `player << 16 \| win`; `0xFF0F` ends the bonus game |
| 31 | 1 | default settings, please (→ 10) |
| 34 | 2 (4P, M107 3) | bonus game hits `[player, small fish, jackpot fish]` (M101–M105 pack the counts as bytes) |

| Chip → game | Words | Meaning |
| --- | --- | --- |
| 2 | 10 | inputs: word `j` bit `b` is key `8j+b` |
| 4 | 19 | version: 8 chars, 8 chars, fuse, two words |
| 6 | 12 | per seat a coin flag (shows a "b" after the credit) and the credits |
| 10 | 19 (4P, M107 37) | settings |
| 12 | 232 (4P, M107 250) | records (`RecordPC`) |
| 14 | 4 | catches: `player << 16 \| weapon << 8`, caught per kind (bit 7 = a jackpot marker) |
| 15 | 40 | errors: 4 master, 6 × 6 per seat |
| 18 | 12 | payout still to go: coins per seat, tickets per seat |
| 20 | 1 | reset done |
| 23 | 12 | chance game per seat (1 tiny, 3 small, 4 big bomb, 5 bonus) and its win |
| 26 | 7 | bonus game: jackpot fish per seat, small fish count (word 6) |
| 35 | 3 | bonus catches `[player, small fish, jackpot multiplier]` |

## What the plugin rebuilds

The chip's logic is reconstructed from the game's side; every number
below is invented:

- **Catches:** each fish in a request is caught with probability
  `RTP / value` (fish values from the game's table: 1, 2, 4, 3, 20, 50, 70,
  30, 40, 10, 5, 100 per kind); fish the game marks as due go at 50%; a
  bomb catches each fish with four times the odds.  The win, value × bet, is credited at once.
- **Bets** come off the credits as message 5 reports them.
- **Chance games:** one shot in `FISH_CHANCE` picks one of tiny game 40%,
  small game 30% (win 20–100 × bet), big bomb 20%, bonus game 10% (every
  seat in play, 1–3 jackpot fish each, 40 small fish).  Wins the game
  reports at the end are credited × the last bet; bonus small fish pay 10,
  jackpot fish 100 × a 1–3 multiplier.
- **Settings** are kept as the game sends them; defaults in the menu's
  ranges, sound and music on.  **Records** count coins, key-in, payouts,
  games, bet and win per seat and the on and play times.

## Useful addresses (M102CN, symbols kept)

| Address | What |
| --- | --- |
| `0x80643F0` | `iPcShareRam_MainInit`: loader, ISR, first batch |
| `0x8063440` | `vPcShareRam_CmdTo36`: game → chip messages |
| `0x8063C90` | `_vCmdProc`: chip → game messages |
| `0x806D010` | `WeaponClass::vCheckGetFish`: applies message 14 |
| `0x806D650` | `WeaponClass::vCheckNetFish`: builds message 13 |
| `0x8085170` | `SettingSysSetting`: the settings page |
| `0x8082D90` | `SettingSysTotalBook`: the records page |
| `0x8245340` | fish table, 36 bytes per kind, value at +10 |
