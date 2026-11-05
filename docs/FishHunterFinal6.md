# Fish Hunter Final 6 (IGS E2100)

终极版猎鱼高手: a shooting fish game for four or six stations around one
screen.  Each station turns its cannon, picks a bet and fires; caught fish
pay their value × the bet.  Specials: the fish king (炫风鱼, catches its
whole kind), the bomb crab (炸弹蟹), the extra-fish chain (送燈, started by
a lightning fish) and the big fish (shark, killer whale, humpback whale,
mermaid, treasure turtle).  The IGS036 decides every catch and holds the
credits; a Sentinel USB dongle holds the channel keys.

Status: playable through the loader on both front ends (plugin
`games/fishhunterfinal6`).  The chip's firmware is not dumped: its game
logic is rebuilt in the plugin (below), so odds and payouts are not the
cabinet's.  The dongle is emulated at its client API.

## Versions

One program, U102CN, in three builds (stripped):

| Version | Id (`hash/e2100.xml`) | Kernel, ROFS profile | Stations | Executable |
| --- | --- | --- | --- | --- |
| U102CN `fish6` | `fishhunterfinal6_fish6` (parent) | `Fish-OS`, `fish` | 6 | 2,631,776 bytes, `asic36.bin` |
| U102CN `fish6p` | `fishhunterfinal6_fish6p` | its own, `fish6pv2` | 6 | 3,157,008, encrypted on disk, chip image embedded |
| U102CN `fish44` | `fishhunterfinal6_fish44` | `Fish4P-OS` #3, `fish4p` | 4 | 3,157,032, chip image embedded |

The station count is a setting (word 48); a build only sets the default
(`FISH_STATIONS`).

## The disk

| Partition | `fish6` (LBA, sectors) | `fish6p` | `fish44` | Contents |
| --- | --- | --- | --- | --- |
| p1 | 63 + 64,197 | 63 + 24,129 | 63 + 48,321 | ext2 boot: LILO, the kernel |
| p2 | 64,260 + 385,560 | 24,192 + 352,800 | 48,384 + 352,800 | `igs_rofs` root: `/Fish.bin/fish` (`fish6p`: `/Fish6P/fish`, encrypted), `asic36.bin` |
| p3 | 530,145 + 3,180,870 | 436,464 + 3,282,048 | 460,656 + 3,257,856 | SquashFS 4: `action/`, `sound/`, `system/`; at `/Fish` (`fish`, `FH` → `../Fish.bin/fish`), `fish6p` at `/Fish6P` (`FH6P` → `../Fish6P.bin/fish`) |
| p4 | 449,820 + 80,325 | 376,992 + 59,472 | 401,184 + 59,472 | ext2, empty: `ScreenCutOff` |

`fish6p` boots like Fish Hunter 6 M607CN: the kernel decrypts the
executable into a tmpfs (`/usr/bin/info … Hello-Fish6PV2.0`; the set's
`decrypt` step with the list's `exe_mask`) and `/etc/X11/IGS` copies
`asic36.bin` there.  All three builds' sounds and animations are
FPGA-encrypted (`asset_mask`).  Keys: `IGS_KEYS.md`.

The game reads the device ids of four PCI cards (libpci) into its FPGA
key; the Windows loader provides an empty PCI bus, the Linux loader the
host's libpci.  Fatal errors go to `ScreenCutOff/<date>.log` (Big5) with a
screenshot.

## Controls

Keys go to the game window.  Station `p` uses input bits `8p+0..5` (up,
down, right, left, bet, fire); Test is bit 78.  With four stations only
the first four rows apply.

| Station | Turn left / right | Up / Down | Fire | Bet | Coin | Key-in | Key-out | Payout |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | ← / → | ↑ / ↓ | Ctrl | Alt | 5 | F5 | Q | F9 |
| 2 | D / G | R / F | A | S | 6 | F6 | W | F10 |
| 3 | J / L | I / K | B | V | 7 | F7 | E | F11 |
| 4 | keypad 4 / 6 | keypad 8 / 2 | keypad 0 | keypad . | 8 | F8 | T | F12 |
| 5 | Delete / Page Down | Home / End | Insert | Page Up | 9 | - | U | [ |
| 6 | H / M | Y / N | X | Z | 0 | = | O | ] |

- **Coin:** 1 coin = the coin rate's credits (1..1000).  Coins and key-in
  stop at the credit-in limit.
- **Key-in / key-out** (开分 / 洗分): the rate's credits per press, when
  开洗分功能 is on (the plugin's default).
- **Payout** by the business mode, as Fish Hunter 6: 0 wins out as coins,
  1 coins on the payout key, 2 wins out as tickets, 3 tickets on the payout
  key (the default), 4 wins out as coins, then tickets.  Tickets at the
  ticket rate (1..1000 credits per ticket).  With 退币键功能 on, holding a
  station's Bet 2 s is its payout key.  The station holds while its
  payout runs, 10 a second.
- **F1** shows or hides the key help.
- **F2** opens the operator menu (accounts, test, system) when no station
  has credits; its keys are station 1's (Up/Down move, Fire enters, Bet
  leaves).  The system pages ask for a 12-digit password: Fire twelve
  times (the stored digits, zeros by default).  The page also takes the
  built-in `860222995048`, or a code for the challenge it shows (sets the
  difficulty ceiling; `IGS_KEYS.md`).
- Station 1's Fire held 3 s on the current book closes the period;
  station 2's Bet and Fire held show the hidden book rows.

## Tunables

| Variable | Default | Meaning |
| --- | --- | --- |
| `FISH_RTP` | 0.95 | expected return of a shot at difficulty 2; each level up takes 0.02 off |
| `FISH_CHAIN` | 0.85 | chance the extra-fish chain goes on each round (at most 20 fish) |
| `FISH_STATIONS` | 6 (`fish44`: 4) | stations the cabinet has (settings word 48) |
| `FISH_CREDITS` | 0 | starting credits per station |
| `FISH_FORCE` | 0 | [Testing] 1: a hit lightning fish always starts the chain, 2: a hit fish king always catches, 3: both |

## The dongle

A SafeNet Sentinel Keys USB key; the game links the client library and
talks to the key through `/var/run/sentinel/u.daemon` (`/usr/sbin/usbdaemon`).
The key's protocol is end-to-end encrypted, so the plugin replaces the
client's API functions in the executable (`igs_core.hook`, both loaders)
with an emulated key: `SFNTGetLicense` (developer `0x5E07A0E1`), then
`SFNTReadRawData` of data object 49 in 16-byte blocks and `SFNTDecrypt` with
its AES feature 43.  The plaintext:

| Bytes | Use |
| --- | --- |
| 0..15 | FPGA key (the emulated FPGA uses the list's mask) |
| 16, 20 | first channel keys: game → chip, chip → game |
| 24 | the running sum's start |
| 28, 32, 36 | `0x100002D0`, `0x10000300`, `0x10000320`: `fish6` hangs without them |
| 64..75 | fish of kinds 0..11 kept on screen in the opening scenes (≤ 39) |

Without the key the game hangs at boot.

## Protection and runtime channel

The "e2k1" ShareRam protocol of Fish Hunter 4 and 6 (`FishHunter6.md`):
batches of messages, `header = type | length << 7`, body `[running sum,
payload, headers]` (the sum shared by both directions), every word XORed
with a 32-bit key per direction.  The game announces its next header
register and key in message 5; the chip its next register in message 6
word 12 and, once, its key in message 37.  The P2A loader compares DONE's
word 1 with the upload's CRC (`E2100_HS_NOSUM`); `fish44` and `fish6p`
carry the chip image in the executable.  The game sends only in answer to
the chip and stops (`jmp $`) after 180 frames without a batch, at a sum
mismatch, or at an unknown message with a payload.

| Game → chip | Words | Meaning |
| --- | --- | --- |
| 3 | 1 | version, please (→ 4) |
| 5 | 11 | every batch: state (0 play, 1 menu, 2 boot), one shot's bet per station, date `yyyymmdd`, 0, next header register, next key |
| 7 | 1 | settings, please (→ 10) |
| 8, 11 | 1 | records, please (→ 12) |
| 9 | 50 | new settings (7 follows) |
| 13 | 12 | catch: bet, `player << 16 \| bullet`, 20 bytes hit per kind (bit 7: a fish king among them; byte 19: 1 + the kind of a lightning fish), 20 bytes chain size per king kind (→ 14) |
| 16 | 34 | lamps (30), counter pulses (4) |
| 19 | 1 | reset all (→ 20) |
| 24 | 6 | the bomb's blast: player, 20 bytes hit per kind (→ 43) |
| 26 | 1 | output test `station << 16 \| action` |
| 28 | 1 | default settings, please (→ 10) |
| 29 | 1 | close the period (→ 12) |
| 39 | 2 | the PC's MAC address |
| 41 | 3 | `fish44`, `fish6p`: three random words at boot (no answer read) |
| 42 | 3 | win report: `player << 8 \| type` (0, 2 big fish, 1 bomb crab, 3 chain), bet, win |
| 44 | 6 | the chain's next fish: player, 20 bytes candidate kinds (→ 45) |
| 46 | 1 | debug |
| 52 | 6 | fish-king win per station |

| Chip → game | Words | Meaning |
| --- | --- | --- |
| 1 | ≤ 24 | fatal message (the game halts) |
| 2 | 4 | inputs: key `8j+b` is byte `j` bit `b`; word 3 DIP switches |
| 4 | 7 | version: fuses, 8 chars, 8 chars, two words |
| 6 | 13 | per station a flag and the credits; next header register |
| 10 | 50 | settings |
| 12 | 934 | records |
| 14 | 8 | catch: `player << 17 \| bullet << 1`, 20 bytes caught per kind (bit 6: the first is a lightning fish starting the chain; bit 7: the king catches its kind; byte 19: the lightning fish starts the chain), M, values of kinds 13, 14, 17 as bytes (× M) |
| 15 | 12 | errors: 4 master, 7 per station (bytes) |
| 18 | 12 | payout still to go: coins per station, tickets per station |
| 20 | 1 | reset done |
| 32 | 6 | push-to-pay per station: the game holds the station (business modes 0, 1, 3) |
| 37 | 1 | the chip's next key (taken once) |
| 43 | 8 | the bomb's catch: `player << 1`, caught per kind, M, values |
| 45 | 2 | the chain's next fish: `player \| kind << 8` (255: the end), value of a shark or killer whale |
| 54 | 114 | per kind (19 × 6 words): may spawn, value |

The game also stores 22, 23, 25, 27, 30, 38 (an FPGA key), 40, 48, 50 and
51 and reads none of them later (51 shows once on a station panel); the
plugin does not send them.  Bit 6 on a fish that is not a lightning fish
stops the game ("非閃電魚啟動送燈"), as does a chain started while one runs.

### Settings

`SettingSysSetting` (系统调整):

| Word | Item | Range |
| --- | --- | --- |
| 0 | 游戏难度 difficulty | 0..7 (..9 with the ceiling in 47) |
| 1, 2, 3 | maximum bet, minimum bet, bet step | 8..1000, 1..1000, 1..50 |
| 4 | coin rate | 1 coin = 1..1000 credits |
| 5 | key-in/key-out | off / on |
| 6, 7 | key-in, key-out rate | 10..10000, 1..10000 credits per press |
| 8 | business mode | 0..4 (see Controls) |
| 9 | ticket rate | 101..128: 1..1000 credits per ticket |
| 10 | credit-in limit | 100,000..500,000 |
| 11 | machine-full limit (系统爆机): the station's warning | 200,000..2,000,000 |
| 12 | payout-key function: Bet held 2 s pays out | off / on |
| 13, 14 | music, sound and voice | off / on |
| 15 | timed fire: an idle station with credits shoots by itself | off / on |
| 16 | volatility (游戏起伏) | low / high |
| 17..25 | hidden (a bet bound, weapon type, score times) | |
| 26..37 | password digits | |
| 47 | difficulty ceiling (a challenge code) | 0..2 |
| 48 | stations: low nibble configured, high requested | `0x44`, `0x66` |
| 49 | mode | |

### Books

`RecordPC` items are pairs `[lo, hi]` = `lo + 100000·hi`, rows of 18:
station `p`'s total, this period at `6 + p`, the last period at `12 + p`.
The total book (总额帐目) shows the totals; the current book (当期纪录)
sums this and the last period; the day book (日报表) the last 42 days.

| Word | Item |
| --- | --- |
| 0, 36 | 总入分 credits in, 总出分 credits out |
| 72, 144 | 总投币 coins in, 总退币 coins out |
| 180, 216 | key-in, key-out |
| 252 | tickets out |
| 288, 324, 360 | played, won (and their ratio), shots (hidden rows) |
| 540, 552 | fish king (炫风鱼): times, won (6 items, total book) |
| 564, 576 | extra-fish chain: times, won (not shown) |
| 588, 600 | bomb crab (炸弹蟹): times, won |
| 636, 637 | play and on time, seconds (shown in hours) |
| 639 | today's day slot (0..41, circular) |
| 640.. | 42 dates `yyyymmdd` |
| 682, 766, 850 | per day `[lo, hi]`: credits in, credits out, tickets out |

## What the plugin rebuilds

The chip's logic is reconstructed from the game's side; the odds are
invented:

- **Fish values** (message 54; the game has none): kinds 0..12 2, 2, 3, 4,
  5, 6, 8, 10, 12, 15, 20, 25, 30; shark 40..120, killer whale 100..250,
  treasure turtle 10..60 (drawn per catch), humpback whale 150, mermaid
  200, bomb crab 20.
- **Catches:** each hit fish is caught with probability `RTP / value`
  (`RTP` by the difficulty; high volatility makes each shot 0.25 or 1.75
  times that); a hit fish king catches its whole kind with `RTP / (value ×
  kind size)`; a hit lightning fish starts the chain with its own catch
  odds.  The bomb crab's blast catches each fish with four times the odds.
  Each chain round adds a random candidate with `FISH_CHAIN`.  Wins (value
  × bet) are credited when the chip answers; the game's reports (42, 52)
  feed the books.
- **Bets** come off the credits as message 5 reports them.
- **Credits, payouts, books:** coins, key-in/key-out, the limits and the
  five business modes as above; every book row, the periods (message 29)
  and the day book by the game's date.  Settings are kept as the game sends
  them; defaults in the menu's ranges.

The lightning fish (field `+20` = 2 in the fish table) is not set by any
code in U102CN, so the chain did not come up in testing.

## Useful addresses (`fish6`)

| Address | What |
| --- | --- |
| `0x80695E0` | `PcShareRam_MainInit`: chip load, dongle, first batch |
| `0x8068580` | game → chip messages |
| `0x8067590` | chip → game messages |
| `0x8067290`, `0x80673D0` | send, receive |
| `0x8070630` | main init and loop |
| `0x806D340` | the dongle thread's Sentinel calls |
| `0x806AB30` | PCI fingerprint |
| `0x80795C0`, `0x80790C0` | catch request, catch reply |
| `0x8072D10` | extra-fish chain start |
| `0x8083D30` | fish spawner |
| `0x80947E0` | `SettingSysSetting` |
| `0x808DEE0`, `0x8090CF0`, `0x80904D0` | total, current, day book |
| `0x8096A30` | password page |
| `0x806E670` | station count (`ChgPNum`) |
