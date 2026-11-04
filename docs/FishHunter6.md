# Fish Hunter 6 (IGS E2100)

A shooting fish game for six stations around one screen: each station
turns its cannon, picks a bet and fires; caught fish pay their value × the
bet.  Chance games (the lucky wheel, the fish-hunt task, the depth bomb)
and the super golden fish for every station in play come up now and then.
The IGS036 decides every catch and holds the credits; the PC draws.

Status: playable through the loader on both front ends (plugin
`games/fishhunter6`).  The chip's firmware is not dumped: its game logic is
rebuilt in the plugin (below), so odds and payouts are not the cabinet's.

## Versions

Fish Hunter 4 M107CN's program (`FishHunter`) for six stations, in two
builds: the Fish-OS CN build (symbols and debug info kept; M601–M604 pack
the golden-fish counts as bytes, M606 has M107CN's word messages and
push-to-pay) and `Fish6Pv2` with Harpoon Lagoon's channel.

| Version | Id (`hash/e2100.xml`) | Kernel, ROFS profile | Variant | Executable |
| --- | --- | --- | --- | --- |
| M606CN | `fishhunter6_m606cn` (parent) | `Fish-OS`, `fish` | `cn2` | 2,534,353 bytes |
| M601FA | `fishhunter6_m601fa` | `Fish-OS`, `fish` | `cn` | 2,534,286 |
| M602CN | `fishhunter6_m602cn` | `Fish-OS`, `fish` | `cn` | 2,534,410 |
| M603CN | `fishhunter6_m603cn` | `Fish-OS`, `fish` | `cn` | 2,534,286 |
| M604CN | `fishhunter6_m604cn` | `Fish-OS`, `fish` | `cn` | 2,534,286 |
| M607CN | `fishhunter6_m607cn` | `Fish6Pv2-OS`, `fish6pv2` | `6p` | 2,698,336, stripped, encrypted on disk |

## The disk

| Partition | M60x (LBA, sectors) | M607CN | Contents |
| --- | --- | --- | --- |
| p1 | 63 + 64,197 | 63 + 24,129 | ext2 boot: LILO, the kernel |
| p2 | 64,260 + 385,560 | 24,192 + 352,800 | `igs_rofs` root; M60x `/Fish.bin/fish`, `asic36.bin`; M607CN `/Fish6P/fish` (encrypted), `asic36.bin` |
| p3 | 530,145 + 3,180,870 | 436,464 + 3,282,048 | SquashFS 4: `action/`, `sound/`, `system/`; M60x mounted at `/Fish` (`fish -> ../Fish.bin/fish`), M607CN at `/Fish6P` (`FH6P -> ../Fish6P.bin/fish`) |
| p4 | 449,820 + 80,325 | 376,992 + 59,472 | ext2, empty: `ScreenCutOff` (`setting.dat`) |

`/etc/X11/IGS` mounts p3 and p4, starts Xorg and loops `./fish` (M607CN
`./FH6P`).

**M607CN** encrypts its executable: at boot the kernel mounts a tmpfs at
`/Fish6P.bin` and runs `/usr/bin/info /Fish6P/fish /Fish6P.bin/fish
Hello-Fish6PV2.0`, which decrypts it with the FPGA cipher.  The set does the
same (`decrypt` with the list's `exe_mask`).  The firmware is embedded, but p3's
`asic36.bin -> ../Fish6P.bin/asic36.bin` must resolve: the game's path scan
(`zlPath::AutoSubPath`) ignores a failed `stat` and would open the dangling
link as a directory, so the set copies p2's `asic36.bin` there.

Its sound loader (`0x8062E10`) hands `SDL_RWFromMem` a stale stack word as
the size; the WSL core gives such a `RIFF` buffer its allocation's size.  Its sounds and animations are FPGA-encrypted
(`asset_mask`).  Keys and constants: `IGS_KEYS.md`.

## Controls

Keys go to the game window.  Station `p` uses input bits `8p+0..5` (up,
down, right, left, bet, fire), Test is bit 78.

| Station | Turn left / right | Up / Down | Fire | Bet | Coin | Key-in | Key-out | Payout |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | ← / → | ↑ / ↓ | Ctrl | Alt | 5 | F5 | Q | F9 |
| 2 | D / G | R / F | A | S | 6 | F6 | W | F10 |
| 3 | J / L | I / K | B | V | 7 | F7 | E | F11 |
| 4 | keypad 4 / 6 | keypad 8 / 2 | keypad 0 | keypad . | 8 | F8 | T | F12 |
| 5 | Delete / Page Down | Home / End | Insert | Page Up | 9 | - | U | [ |
| 6 | H / M | Y / N | X | Z | 0 | = | O | ] |

- **Coin:** credits by the "coin rate" (1 coin → 1, 2, 3, 5 or 10 credits,
  or 2 coins → 1).
- **Key-in / key-out** (开分 / 洗分): the attendant's keys, the rate's
  credits per press, when "key-in/key-out" is on (the plugin's default).
- **Payout** by the business mode: 0 wins out as coins, 1 coins on the
  payout key, 2 wins out as tickets, 3 tickets on the payout key (the
  default), 4 wins out as coins, then tickets.  Coins go at the coin rate;
  tickets at the ticket rate (1..50 credits per ticket, or 10/20/30 tickets
  per credit).  With "bet-key coin-out" on, holding a station's Bet key 2 s
  is its payout key.  The station holds while its payout runs, 10 a second.
- **F1** shows or hides the key help.
- **F2** opens the operator menu (accounts, test, system) between games;
  its keys are station 1's (Up/Down move, Fire enters, Bet leaves).  The
  system pages ask for the 8-digit password (default 00000000: Fire eight
  times).

## Tunables

| Variable | Default | Meaning |
| --- | --- | --- |
| `FISH_RTP` | 0.95 | expected return of a shot at difficulty 5; each level up takes 0.02 off |
| `FISH_CHANCE` | 300 | one shot in n starts a chance game; 0 = never |
| `FISH_CHANCE_KIND` | 0 | [Testing] 0 random, 1 lucky wheel, 3 fish-hunt task, 4 depth bomb, 5 super golden fish |
| `FISH_CREDITS` | 0 | starting credits per station |

## Protection and runtime channel

Both builds run Fish Hunter 4's "e2k1" ShareRam protocol
(`FishHunter4.md`): batches of messages, `header = type | length << 7`.

| | M601–M606CN (`cn`, `cn2`) | M607CN (`6p`) |
| --- | --- | --- |
| Body | `[payload, headers, sum]` | `[running sum, payload, headers]`, the sum shared by both directions |
| Header register | BAR1 0, both ways | the game's: announced in message 5 word 11 (0..63); the chip's: message 6 word 12 |
| Cipher | RC4 per direction over the body, set once: `0x82675251` (chip → game), `0x80678267` | each word XOR a 32-bit key per direction, same initial keys; the game's next key in message 5 word 12, the chip's in message 42 (taken once) |
| Boot | P2A loader, `asic36.bin` | the same loader, the embedded image (upload key `PC Card 0 Opened`) |
| Fish kinds per catch | 12 (3 words of bytes) | 13 (4 words) |

Both loaders compare DONE's word 1 with their upload's CRC: with nothing
uploaded, 0 (`E2100_HS_NOSUM`).  The `6p` game stops at a checksum
mismatch, and after 180 batches without an answer outside the operator
menu.

| Game → chip | Words (`cn` / `6p`) | Meaning |
| --- | --- | --- |
| 3 | 1 | version, please (→ 4) |
| 5 | 11 / 13 | every batch: game state, bet per station since the last batch (`bet`, `bet + 100` for the idle auto-shot, 99 for the star bomb), date `yyyymmdd`, chance-max flag, station count, 0; `6p`: next header register, next key |
| 7 | 1 | settings, please (→ 10) |
| 8, 11 | 1 | records, please (→ 12) |
| 32 | 1 | close the period (the current book's Fire key held 3 s), then the records |
| 9 | 37 | new settings (`SettingPC`) |
| 13 | 7 / 9 | catch request: `player << 16 \| weapon << 8 \| bet (+100 boosted)`, fish per kind the game's damage count says are due, fish hit per kind (bytes) (→ 14) |
| 25 | 7 / 9 | the same for a bomb (no hints) |
| 16 | 16 | lamps and counters |
| 17 | 3 | payout set-up |
| 19 | 1 | reset all (→ 20) |
| 24 | 2 | the swell fish was hit `[player, level]` |
| 27 | 2 | big-game win report |
| 28 | 1 | chance game over: `player << 16 \| win`; `0xFF0F` ends the golden fish |
| 29 | 1 | test data |
| 31 | 1 | default settings, please (→ 10) |
| 34 | 3 (`cn`: 4) | golden-fish hits `[player, small fish, jackpot fish]`; `cn`: `[player, small \| jackpot << 8, 0, 0]` |

| Chip → game | Words (`cn` / `6p`) | Meaning |
| --- | --- | --- |
| 2 | 10 / 3 | inputs: key `8j+b` is word `j` bit `b` (`cn`) or byte `j` bit `b` (`6p`) |
| 4 | 19 / 5 | version: 8 chars, 8 chars, fuse, two words (`6p`: the last three as bytes of word 0, the strings as bytes) |
| 6 | 12 / 13 | per station a coin flag (a "b" after the credit) and the credits; `6p`: the chip's next header register |
| 10 | 37 | settings |
| 12 | 420 | records (`RecordPC`, with the day book) |
| 14 | 4 / 5 | catches: `player << 16 \| weapon << 8`, caught per kind (bytes) |
| 15 | 40 / 12 | errors: 4 master, 6 per station (`6p`: bytes, 7 per station) |
| 18 | 12 | payout still to go: coins per station, tickets per station |
| 20 | 1 | reset done |
| 23 | 12 | chance game per station (1 lucky wheel, 3 fish-hunt task, 4 depth bomb, 5 golden fish) and the task's prize |
| 26 | 7 | golden fish: jackpot fish per station, small fish count (word 6) |
| 35 | 3 (`cn`: 4) | golden-fish catches `[player, small fish, jackpot multiplier]`; `cn`: `[player, small \| multiplier << 8, 0, 0]` |
| 37 | 6 | `cn2`, `6p`: push-to-pay per station: the game holds the station (business modes 0, 1, 3) |
| 42 | 1 | `6p`: the chip's next key |
| 43 | 16 | `6p`: the asset key, a byte per word, for the FPGA (the loader's cipher takes the list's mask) |

The game also accepts 22 (catch statistics), 30, 33 and 36 and stores
them without reading them; the plugin does not send them.

### Settings

`SettingPC`, the system page (`SettingSysSetting`):

| Word | Item | Range |
| --- | --- | --- |
| 0 | difficulty | 1..10 (..20 with the chance-max flag) |
| 1 | level (hidden) | 0..2 |
| 2, 3 | maximum, minimum bet | 1..8, 1..3 |
| 4 | coin rate | 0..5 (see Controls) |
| 5 | bullet speed | normal, fast |
| 6 | score times (hidden) | 0..2 |
| 11 | bet-key coin-out | off / on |
| 12 | key-in/key-out | off / on |
| 13, 14 | key-in, key-out rate | 10..500, 1..500 credits per press |
| 15 | business mode | 0..4 (see Controls) |
| 16 | ticket rate | 1..50, 101..103 |
| 17, 18 | music, sound | off / on |
| 19 | on/off shown in other areas | |
| 20..27 | password digits | |

### Books

`RecordPC` is 18-word rows: station `p`'s total at word `p`, this period
at `6 + p`, the last period at `12 + p`.  The total book (system) shows
the totals per station; the current book (accounts) sums this and the last
period; holding station 2's Bet and Fire shows the rows after 126.

| Row | Item |
| --- | --- |
| 0, 18 | credits in, credits out |
| 36, 72 | coins in, coins out |
| 90, 108 | key-in, key-out |
| 126 | tickets out |
| 144, 162 | played, won (and their ratio) |
| 180 | shots |
| 198, 204 | lucky wheel: times, won (6 words each, total book only) |
| 210, 216 | fish-hunt task |
| 222, 228 | depth bomb |
| 234, 240 | super golden fish; 246, 247 beside their totals: jackpot fish, their win |
| 248, 249 | play and on time, seconds |
| 251 | days kept − 1 (day book) |
| 252.. | 42 dates `yyyymmdd`, newest first |
| 294, 336, 378 | per day: credits in, credits out, tickets out |

## What the plugin rebuilds

The chip's logic is reconstructed from the game's side; the odds are
invented:

- **Catches:** each fish in a request is caught with probability
  `RTP / value` (fish values from the game's table: 1, 2, 4, 3, 20, 50, 70,
  30, 40, 10, 5, 100, 10 per kind); fish the game marks as due go at 50%; a
  bomb catches each fish with four times the odds.  The win, value × bet,
  is credited at once.
- **Bets** come off the credits as message 5 reports them.
- **Chance games:** one shot in `FISH_CHANCE` picks the lucky wheel 40%,
  the fish-hunt task 30% (prize 20–100 × bet, shown by the game), the depth
  bomb 20%, the super golden fish 10% (every station in play, 1–3 jackpot
  fish each, 40 small fish).  Wins the game reports at the end are credited
  × the last bet; golden small fish pay 10, jackpot fish 100 × a 1–3
  multiplier.
- **Credits, payouts, books:** coins, key-in/key-out and the five business
  modes as above; every book row, the periods (message 32) and the day
  book by the game's date.  Settings are kept as the game sends them;
  defaults in the menu's ranges.

## Useful addresses

| Address | What |
| --- | --- |
| M606 `0x8064630` | `iPcShareRam_MainInit` |
| M606 `0x8063460` | `vPcShareRam_CmdTo36`: game → chip messages |
| M606 `0x8063D90` | `_vCmdProc`: chip → game messages |
| M606 `0x8085A40` | `SettingSysSetting` |
| M606 `0x80835A0`, `0x8081FE0`, `0x8081900` | `SettingSysTotalBook`, `SettingBookNow`, `SettingBookDay` |
| M606 `0x820C300` | `CoinRate` |
| M606 `0x80689E0` | `PlayerClass::CheckError`: errors, payout panel, push-to-pay |
| M606 `0x8248040` | fish table, 36 bytes per kind, value at +10 |
| M607 `0x8065350` | channel start-up: loader, keys |
| M607 `0x808BB30` | `P2A_LoaderAdv_iLoadDataPointer` |
| M607 `0x8063BF0` | game → chip messages |
| M607 `0x8064620` | chip → game messages |
| M607 `0x8064340`, `0x8064480` | send, receive |
| M607 `0x80839E0` | the day book |
| M607 `0x82CC060` | fish table |
