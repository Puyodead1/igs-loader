# IGS E2100 FPGA / ASIC protection protocol

## Verdict

The protection can be high-level emulated, as the loader's E2100 machine
does.  The game never verifies that the protection ASIC is genuine:

- The boot handshake is a fixed command state machine driven by the ASIC.  An
  emulated ASIC can skip the firmware upload and go straight to "done".
- The runtime channel uses an additive checksum, and each direction XORs every
  word with a single 32-bit key.  Each side announces its next key in-band.
- No message carries a challenge that needs a secret to answer.

`asic36.bin` (512 KB, entropy 7.9996) is the ASIC firmware image.  The game
uploads it only if the ASIC asks for it (command `0xD0D0D0D0`), so it is not
needed for HLE.

Sources (Harpoon Lagoon M107US): the game `/Fish.bin/fish` (user-space
library `FPGA_Comm_*` / `FPGA_AES_*`, start-up `sub_8067F10`) and the
decompressed `Fish4P-OS` kernel (`FPGA_iIoctl` `0xC1021E0D`, ISR
`0xC1021B94`).  The kernel driver is part of the
E2100 OS, so other E2100 games are expected to use the same protocol; the
`fish` addresses are specific to this build.

## Hardware interface (kernel driver)

`io` = BAR3 I/O port base (`dword_C1CB4BE8`).

| Port | Access | Meaning |
| --- | --- | --- |
| `io+3` | out `0x40` | ISR acknowledgement of a `0x40` event |
| `io+4` | out `1` | ioctl `0x6601`: host posted a message (notify ASIC) |
| `io+7` | in / out W1C | interrupt status: `0x10` (wakes a wait queue), `0x20`, `0x40` = ASIC event |
| `io+8` | out `1` | ioctl `0x6600`: reset ASIC |
| `io+0x10` | outl | DMA length |
| `io+0x13` | outb | DMA command: `0x11` = host→SRAM, `0x51` = SRAM→host |
| `io+0x17` | in / out | DMA status: bit 0 busy (polled before start); bit 4 done (W1C, raises the IRQ) |
| `io+0x18` | outl | SRAM address |
| `io+0x1C` | outl | physical address of the host buffer; the descriptor word at +4 is `len \| 0x80000000` |
| `io+0x20..0x2F` | out | 16-byte crypto key |
| `io+0x40` | out | crypto/mode byte (`0x84 \| 1/2` for the AES ioctl, `5`, `6`, `0x85`, `0x86` for `0x6607/9/B/D`, `0` = off) |

The ISR reads `io+7`.  For bit `0x10` it acks, clears `C1CB4C10` and wakes up
the waiter.  For `0x20` it acks.  For `0x40` it acks, increments the event
counter (read by ioctl `0x80046603`) and writes `0x40` to `io+3`.  It then reads
`io+0x17`; if bit `0x10` is set it acks, clears the DMA-busy flag
`C1CB4C0C` and wakes up the waiter.

The asset ioctl `0xC0106610` (`sub_C102183C`) has five steps:
1. Write the key to `io+0x20`.
2. Write the mode to `io+0x40`.
3. DMA host→SRAM with command `0x11`.  The FPGA applies the cipher here.
4. Write 0 to `io+0x40`.
5. DMA SRAM→host with command `0x51`.

The cipher is the one documented in `IGS_SOUND_CRYPTO.md`.

The game maps BAR0 (`0x80000`) and BAR1 (`0x800`) with `mmap` ("polling and pio
mode").  A user-space thread polls ioctl `0x80046603`.  When the counter
changes, it calls the top callback.

## Mailbox (boot handshake)

BAR1 bytes `0x100..0x10F` are four dwords.  The read and write sides are
separate: the host writes its outgoing message, then reads the ASIC's reply from
the same offsets.  The ASIC must latch the outgoing words when the host
writes the notify port (`io+4`).  The host overwrites them with `0, 0, rand,
rand` before each read.

- `send(a, b)`: `[0x100]=a, [0x104]=b, [0x108]=rand, [0x10C]=rand`, then notify.
- `recv()`: `send(0,0)` without notify, then read `[0x100], [0x104]`.
- `wait(n)`: wait up to n seconds for the next `0x40` event.

### Sequence (`sub_80A1B00`, then `sub_80A2250`)

1. ioctl `0x6600` (reset), then `wait(20)`.  The ASIC must raise a `0x40` event.
   The handoff notes show the first IRQ must have status `0x10|0x40`.
2. `recv() -> (a, b)`.  The host sends `(a, a ^ crc32(a))` (the table CRC in
   `sub_80A2710`), then `wait(5)`.  The ASIC must raise `0x40`.  The content
   of the hello is never checked.
3. The host sends `(r ^ 0x08080808, r)` and four random session keys at
   `0x108, 0x10C, 0x110, 0x114`, then notifies.
4. Loop: `wait(10)`, then `recv() -> (c, d)`.  On the first pass the host finds the
   key `k` for which `c ^ k == 0xF0F0F0F0`.  That `k` becomes the session mask, and
   every later word in both directions is XORed with `k`.  It then handles
   `c ^ k`:

| ASIC command | Host reply (masked) | Notes |
| --- | --- | --- |
| `0xF0F0F0F0` | `0x0A0A0A0A, rand` | retry/hello; at most 5 |
| `0xB0B0B0B0` | `0x0D0D0D0D, rand` | |
| `0xC0C0C0C0`, `0xA0A0A0A0`, `0x70707070`, `0xE0E0E0E0` | `0x08080808, rand` | |
| `0xD0D0D0D0` | upload | see below |
| `0x90909090` | `0x0C0C0C0C, rand` | first runs a dummy 64 KB crypto ioctl (`0xC0106610`, key `"298yvan42o 89phq24rl;"`), which must succeed; then **the host returns success** |
| other | — | fails with 4523 |

The upload has four steps:
1. The host decrypts each 64 KB page of the embedded image with the FPGA
   cipher and the key `"Open pccard0 !!!"`, and computes its CRC.
2. The pages go in a shuffled order.  For each one the host sends
   `(0x12345678, page)` and expects `(0x87654321, crc)` back.
3. If all eight CRCs match, the host writes the raw image to BAR0 and sends
   `(0x0B0B0B0B, size)`.
4. On a mismatch it retries up to 2 times, then fails.

5. After the handshake returns, `sub_8067F10` does `wait(1)`.  So the ASIC
   must raise one more `0x40` after receiving `0x0C0C0C0C`.  The game then
   installs the runtime callback `sub_8065F40`, posts an empty batch, and blocks in
   `sub_8066390(0)` until the first runtime event.

**Minimal HLE:**
- On reset: raise `0x10|0x40`.
- On every notify: raise `0x40`.
- After the key notify: reply `0xF0F0F0F0 ^ k`, where `k` is the word written
  at `0x108`.
- After the host replies `0x0A0A0A0A`: reply `0x90909090 ^ k`.
- After `0x0C0C0C0C`: raise one more `0x40`.

## Runtime message channel

Messages are 32-bit words.  A batch is identified by a header register: BAR1
dword `idx` (offset `4*idx`, `idx` < 64), where
`header = (payloadWords << 7) | nMessages`.  The batch body is at BAR0 offset 0.

### ASIC → game (`sub_8066590`)

1. Read the header from BAR1 dword `rxIdx`.  `rxIdx` starts at 0 and is
   changed by message type 6.
2. Read `n = nMessages + payloadWords + 1` dwords from BAR0 offset 0.
3. XOR every word with `rxKey`.  `rxKey` starts at `0x82675251` and is
   changed by message type 43.
4. The decrypted layout is `[checksum, payload[payloadWords], msgHeaders[nMessages]]`.
5. The checksum must equal `running + sum(payload) + sum(msgHeaders)`, and
   `running` then becomes the checksum.  **On a mismatch the game spins
   forever.**
6. Each message header is `(len << 7) | type`.  The messages consume
   `payload` in order through `sub_8066730(type, len)`.

### Game → ASIC (`sub_8066450`, after each received batch)

1. Write the header `(payloadWords << 7) | nMessages` to BAR1 dword `txIdx`.
2. Build the body `[checksum, payload..., msgHeaders...]` with the same
   running checksum, which is shared by both directions.
3. XOR it with `txKey` and write it to BAR0 offset 0, then notify.

The game picks its next `txIdx` (0..63) and `txKey` (24-bit random) and sends
both in its message type 5.  The 13-word payload starts at `0x8402E20`: word 11
is the next index and word 12 is the next key.  The first key is `0x80678267`
and the first index is 0.

### ASIC → game message types (`sub_8066730`)

| Type | Effect |
| --- | --- |
| 1 | prints a 24-word string and calls an error path (`sub_8066710`), then falls into 2 |
| 2 | 3 values → `dword_8405420` table |
| 4 | 3 bytes + up to 2 groups → `dword_8402E00..`, `dword_8402DD4..`, `dword_8402DE0..` |
| 6 | settings block → `0x8405180`; word 12 = new `rxIdx` |
| 10, 12, 18, 23, 26, 30, 33, 36, 38, 52, 55, 61 | raw blocks copied to globals |
| 14 | 4-byte record + `n` 13-byte records → 30-slot ring at `0x8402EC0` |
| 15 | 11-word record → `0x84037E0` table |
| 20 | sets flag `0x8405960` |
| 22 | 7-word record → `0x8405240` table |
| 35 | 3 bytes: index, two values → `0x84030DB` table |
| 43 | new `rxKey` (if armed) |
| **44** | **16-word asset key → `byte_84059A0`** |
| 59 | increments counter `0x8405AB8[n]` (n ≤ 5) |
| 39, 40 | ignored |
| other | counted as unknown (`dword_840597C`) |

`sub_806AFB0` copies the type-44 key into the asset-decryption key at start-up.
With an emulated FPGA that applies the known cipher, the key value does not matter.

## Implementation status

The HLE is implemented in `machines/e2100/` and `games/harpoonlagoon/`,
and the game plays.  Findings from bring-up:

- **Settings (message 55).**  The game needs them before gameplay code runs:
  word 10 is a divisor (SIGFPE at `0x0806DCB8` when it is 0).  The block at
  `0x8405A00` is an indexed settings array.  The getter `sub_806B760` returns
  word N, except for N = 9 and 10, which come from the per-player message-52
  table.  Words 7/0 are shown as a "%d ~ %d" range and word 8 as
  "%d tickets"; words 11..14 are per player.  At least 15 words.
- **Message 10** fills the second operator settings block `0x8406620`
  (50 words).  The setup menu edits it in place and sends it back as
  message 9; the game has no defaults of its own (the 10-word presets at
  `0x82271A0`.. only cover words 0..9).  **Word 17 = Attract Sound**
  (`dword_8406664`, used by `sub_807CDA0`):
  - Outside attract mode the music gate is always 1.
  - In attract mode: 0 = silent, 1..3 = sound every N-th attract cycle,
    >3 = always.
  - The scene BGM is always started; the gate only sets its volume
    (`sub_807D770`: volume = gate ? 0x80 : 0).
  - The HLE sends word 17 = 4, with everything else 0.

  Unrelated: F3 toggles `byte_840772B`, a debug "show 18 action layers"
  flag (`sub_807C700`); it does not affect sound.
- **Message 52** fills `0x8405A60` with three words per player: an amount, a
  status (0/1/2/3/99), and a value.  All zeros is an acceptable idle state.
- **Game → ASIC types seen** after start-up: 3, 5 (each batch), 7, 28 (one per
  player, `player << 16`), 51, 54, 58 (len 4, one per player), 64 (each batch).
- **Timing.**  The first ASIC runtime batch must follow the game's first
  runtime notify.  If it is sent with the handshake's final event, the game's
  own header-register write overwrites it, and the batch is lost (the
  checksums then disagree by exactly that batch's sum).

## Cabinet controls, coins and credits

These were mapped from the game and are implemented in the HLE.

**Buttons: message 2 (3 words).**
- The 10 low bytes form an 80-bit bitmap.  `sub_8067E90` turns it into hold
  counts at `dword_8405460[k]` every frame.
- "Pressed" (`sub_8069AA0`) means a hold count ≤ 5; "held"
  (`sub_8069A10`) means non-zero.
- Station p owns bits `8p+0..5`: up, down, right, left, bet, fire.  The
  per-player table at `0x82274E0` lists them as `[left, right, up, down,
  fire, bet]`; `0x8227528` maps the same actions to developer SDL keys.
- Left/right aim the crossbow by 2° per frame (±80°).
- Fire (in state 2) shoots and sets `dword_8402E24[p]`, which the type-5
  heartbeat reports.
- Bit 78 is the operator Test button.

**Coins: message 59 `[p]`.**  It increments `dword_8405AB8[p]`, which plays
the "+N" harpoon pop-up and the coin sound (`sub_806D700`).  It is display
only.

**Credits: message 6 (13 words, `dword_8405180`).**
- `[p]`: coins toward the next game.  The station shows `"%da%d"` against
  coins-to-play, which is message-10 word 1 + 1.
- `[6+p]`: games available.  Non-zero moves the station from state 0 to 1.
- `[12]`: the ASIC's next header index (`rxIdx`).

**Game start: game → ASIC message 50 `[p]`** (`sub_8072DF0`).  The game
sets its local message-52 status to 3/3; the ASIC deducts a credit and
answers with message 6 and message 52.

**Per-player state: message 52 (12 words, `[amount, status, value]` per
player).**  The status steers the shot outcome in state 2: 0 = normal,
1/2 = forced hit index, other values = no hit.  The value doubles as
settings 9/10 for that player (`sub_806B760`).

**Game events: game → ASIC message 58 `[p, action, a, b]`.**  Action 0 or 2 =
progress or score; 1 = round over (state 7, unless `dword_840664C`); 3 =
ticket payout (a = ticket setting, b = pounds).

**Watchdog** (`sub_8067E90`): if no ASIC batch is processed for 600 frames
(and `dword_8402E20 != 2`), the game spins forever.

## Ocean Party: a different runtime protocol

The kernel driver, ioctls, mailbox handshake and batch framing are shared
across games.  The cipher and the message set are not.  Keys are in
`IGS_KEYS.md`.

**Unchanged:**
- The boot handshake: the loader's HLE works unmodified.
- The header register: `(payloadWords << 7) | nMessages`.
- The batch body `[checksum, payload..., headers...]` and the shared running
  checksum.

**Channel cipher: RC4 in place of the word XOR.**
- One RC4 context per direction (`sub_806AAF0` key schedule, `sub_806ABD0`
  crypt).
- The key is the 32-bit channel key as 4 little-endian bytes.
- The keystream is XORed byte-wise over each little-endian word.
- The stream continues across batches; it is re-keyed only when a new key
  takes effect.
- Initial keys: ASIC → game `0x364251`, game → ASIC `0xA5148F`.
- An empty ASIC batch (header 0) is skipped without consuming keystream.

**Game → ASIC** (`sub_8067130`, messages built by `sub_8067610`):

| Type | Words | Content |
| --- | --- | --- |
| 8 | 3 | heartbeat, every batch: word 0 = state bits + year, word 1 = date/time + flags, word 2 = next header index (low byte) and next channel key (`>>8`, 24-bit) |
| 9 | 1 | bitmap of SDL keys pressed (developer keyboard) |
| 6 | 1 | 0 |
| 15 | 6 | sent once at start-up |

The game switches to the announced key after sending the batch.

**ASIC → game** (`sub_8068910` receive, `sub_8067C30` dispatch):

| Type | Content |
| --- | --- |
| 1 | error string (24 bytes) → SYSTEM ERROR 01 screen |
| 2 | block → `0x83E4B80` |
| 3 | status bit fields (word 0: 6-bit value, flags, 16-bit value, …; words 3–10: nibble fields); word 11 = next RX index (low byte) and next RX key (`>>8`) |
| 4 | sub-switch on word 0 (1, 3, 4, 6, 7, 9–11, 13–19, 23–27, 29–31, 33–35, 37–39): settings and data blocks |
| 5 | sound command: `0x01xxxxxx` play, `0x02xxxxxx` / `0x03xxxxxx` stop/other |
| 7 | packed bit fields |
| 10 | 128-byte block → `0x83E5CA0` |

**Errors:**
- **SYSTEM ERROR 02** (`sub_8067360`): ASIC batch checksum mismatch.
- **SYSTEM ERROR 01** (`sub_8067500`): reached through `sub_8067280(1)` (and
  message 1).

**The ASIC runs the game.**
- Type 3 sets the game mode: 0 idle, 1 swim, 2 transition, 3 round,
  4 settings.  It also carries music and effects enable, the result history
  and per-seat payout state.
- Type 7 carries the round result.
- The game reports its state back in the type-8 heartbeat.
- Field-level details are in `OceanParty.md`.

**Status in the loader:** RC4 framing works in both directions, and the game
is playable through a re-implemented controller in the shim (betting,
results, payouts).

## Fish Hunter 4: trailing-sum batches

Fish Hunter 4 (2010–2011, the "e2k1" ShareRam of `shareram_e2k1/pcshareram.c`)
keeps the batch messages (`type | length << 7`) with another body:
- **Register:** BAR1 register 0 holds `count | payload << 7` in both
  directions; no index rotation.
- **Body:** `[payload, headers, sum of both]`; no running checksum.  The
  game sends a batch and rings, the ASIC answers one batch.
- **Cipher:** none (M101–M105CN) or RC4 per direction over the whole body
  (M107CN and the 4P build), keys `0x82675251` / `0x80678267` (Harpoon
  Lagoon XORs with the same two words).
- **Boot:** the CN builds' P2A loader sends `0x08080808`, answers "ready"
  (`0xF0F0F0F0`) with `0x0A0A0A0A`, and on DONE (`0x90909090`) compares
  word 1 with the checksum of its upload: with nothing uploaded, 0.  No
  session keys (word 2 of the mailbox stays 0).

**Loader:** the game plugin's `layout = E2100_LAYOUT_TRAILING` and
`handshake = E2100_HS_NOSUM` in `e2100.c`; the messages are in
`FishHunter4.md`.

## Fish Hunter 6

M601FA–M606CN keep Fish Hunter 4 M107CN's channel.  M607CN
(`shareram_e2k1v1.0.5`) moves the same messages onto Harpoon Lagoon's
framing: `[running sum, payload, headers]`, each word XORed with a key per
direction (`0x82675251` / `0x80678267` at start), the game announcing its
next header register and key in message 5 (words 11, 12), the ASIC its
register in message 6 word 12 and its next key in message 42.  Its loader
(`P2A_LoaderAdv_iLoadDataPointer`, image embedded, key `PC Card 0 Opened`)
reads the mailbox without a session XOR and, like the CN loader, compares
DONE's word 1 with its upload's CRC (`E2100_HS_NOSUM`).  Messages:
`FishHunter6.md`.

## Scratch: the register-table transport

Scratch (2012) keeps the boot handshake but replaces the batches with a
64-word table in the BAR1 header registers ("PCSR",
`shareram_e2k1/pcshareram.c`).  The exchange runs once per frame:
1. The game reads all 64 registers (the ASIC's last reply) and zeroes them.
2. It writes its own table and rings.
3. The ASIC replies with one event.

**Table:** word 0 and word 32 are presence bitmasks, words 26/53 are
low-byte checksums, and word 31 marks which words are BAR0 offsets of data
blocks.  There is no cipher and no running checksum.

**Loader:** `e2100.c` keeps separate read and write sides for these
registers, and the game plugin selects the transport
(`E2100_TRANSPORT_REGTABLE`).  The words are listed in `Scratch.md`.

## Hero of Robots: request/response transport

Hero of Robots (2016) also keeps the handshake: the upload key is
`StartHeroOfRobot`, and the image is embedded in the binary.  The runtime is
a third design, strictly one reply per request:
- **Game sends:** BAR1 word 0 = byte length, the bytes at BAR0 offset 0,
  then a ring.
- **ASIC replies:** the same way, and only after a request; the game waits
  for each reply.

- **Interrupts:** the game's poll thread checks the event counter every
  millisecond and calls back only when it reads exactly 1, so the ASIC
  raises exactly one event per request.
- **Watchdog:** more than 3 s from request to reply and the game spins
  forever at the next exchange; message 0 with a non-zero flag clears it.

**Batches:** a 16+ byte header, `[type, length, data]` messages, and a
checksum trailer (low byte = a running byte sum shared by both directions),
all encrypted.  The cipher is a modified RC4: one shared S-box, i/j reset
on every call.  The game re-keys it with message 0 (seeded by `LF KO
IGSA36`) and message 5.

**What the game needs from the ASIC:**
- the pointer of its current state object, echoed back from a table the
  game sends (the state machine runs only through it);
- inputs (message 4, with a byte checksum; a coin counts on every message
  that has the bit);
- settings blocks, version (the region byte), RSA key parts for its smart
  card;
- the key that unlocks its `.osmenc` assets.

**Loader:** `E2100_TRANSPORT_REQRESP` in `e2100.c` (one reply per ring, no
periodic batches); cipher, framing and messages are the game plugin's.  The
full map, the smart card and the serial peripherals are in
`HeroOfRobots.md`.

## Remaining unknowns

- What the game needs from the runtime messages to reach attract mode and accept
  input: buttons, coins and tickets are probably among types 2, 4, 14, 15 and 22.
  Each type's parser is visible in `sub_8066730`, but the meaning of its fields
  still has to be mapped.
- Whether the game times out when no messages arrive.  The message-received flag
  `dword_8405978` is cleared on unknown types, and the main loop has not been
  checked for a watchdog yet.
- The `0x6607`–`0x660E` mode bytes and the `0x4024660F` 16/32-byte key write
  (another crypto mode?) are unused by the paths traced here.
