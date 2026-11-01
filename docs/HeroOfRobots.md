# Hero of Robots m100fa (IGS E2100)

A robot card-battle game, 2015/2016, English (`media/localization/loc.ini`
says `EN`; region byte USA).  Players scan collectable robot, power and skill
cards on a QR reader, fight CPU robots in rounds of melee/shoot/defend, and
buy new cards from the card dispenser with points on a smart card.  It is an
Ogre 1.6.1 game (GLX render system, Cg shaders, Scaleform GFx for the 2D
screens) with SDL only for audio.

**Status (2026-10-30): plays in the loader** (`play.ps1 heroofrobots_m100fa`): boot,
copyright, warning, IGS logo, attract (title, opening movie, ranking), coins,
start, mode/difficulty select, card input, loading, rounds of fighting, and
the end of a game: the smart card is debited, a card is "dispensed", game
over, back to the coin page (a whole game loop seen in a scripted run).  The
ASIC's side and the smart card are emulated; the dispenser and the QR reader
are minimal (always ready; cards scan only from NFC tags, NFC cards).  Start-up is slow:
about 90 s to the copyright screen and 2 minutes for a stage load when the
set is on a Windows drive under WSL (Ogre stats thousands of files over
WSL's 9p mount).

## Playing

```powershell
.\play.ps1 heroofrobots_m100fa        # WSL
.\play-win.ps1 heroofrobots_m100fa    # native Windows: the copyright screen after about 80 s
```

The game's own keyboard handling is off (`KeyMgr::keyPressed` ignores all
but `/`); every input comes from the IO board through the ASIC, so the keys
below are the plugin's.  F1 toggles the key help in the corner.

| Key | Cabinet |
| --- | --- |
| `5` | coin (one credit with the default settings) |
| `X`, Space, Enter, Ctrl | player 1 button 1 (red "Confirm"; starts a game) |
| `Z`, `C` | player 1 buttons 2 and 3 |
| ↑ ↓ | the lever ("Select") |
| `M` `N` `B` | player 2's buttons (guess) |
| F2 | Test (operator menu) |
| `9` | Service (adds a service credit) |
| F1 | show/hide the key help |

To start, a game needs a credit **and** points on the smart card; the
emulated card starts with 100 points (`HERO_CARD_CREDIT`).

## The disk

The image `e2100_heroofrobots_m100fa` is 8,019,099,648 bytes (CHD data
SHA-1 `9ba1d1f99de1585d207736dbc0411671007f97bb`).  In the rom set:

| Cabinet path | What |
| --- | --- |
| `/` | p2 `igs_rofs` root filesystem (profile `heroofrobots`); p1 holds the LILO boot and `HeroOfRobots-OS` |
| `/Robots.bin/child_linux` | the game (89 MB, not stripped, with debug info) |
| `/Robots/` | p3, the game's tree: `exe/` (working directory), `data/` (Ogre configs, `data/so/` Ogre, plugins, Cg, the real `libcasmcard.so`), `media/` (assets, `osmbin/*.osmenc` encrypted), `rewrite/` (Ogre logs) |
| `/Robots/exe/rw_data/` | p4, writable: bookkeeping (`book_combine.bin`, `card_use.bin`), `debit_time.txt`, music, videos |

`/Robots/exe/child_linux` links to `../../Robots.bin/child_linux`.
`/Robots/exe/child.rom` points at `../../Robots.bin/child.rom`, which does not
exist on the image; the binary never names it (a `child.rom` in
`media/localization/` is not used either).

| Partition | Start LBA | Bytes | Content |
| --- | ---: | ---: | --- |
| p1 | 2,048 | 12,582,912 | boot |
| p2 | 26,624 | 188,743,680 | `igs_rofs` root |
| p4 | 395,264 | 1,258,291,200 | `rw_data` |
| p3 | 2,852,864 | 2,456,502,272 | game tree (`/Robots`) |

**Start-up on the cabinet:** `/etc/X11/IGS` mounts p3 on `/Robots` and p4
on `/Robots/exe/rw_data`, starts Xorg and `pcscd`, then loops `cd
/Robots/exe; ./child_linux` with
`LD_LIBRARY_PATH=/Robots/data/so:/lib:/usr/lib:...`.

## Protection

Details of the transport: `IGS_E2100_PROTECTION.md`.  Keys:
`IGS_KEYS.md`.  All addresses below are in `child_linux`.

**Boot handshake:** Harpoon's, unchanged (`P2A_LoaderAdv_iLoadDataPointer`,
upload key `StartHeroOfRobot`, the image embedded as `A36ROM`).  The
loader's HLE skips the upload.

**Runtime: request/response.**  `Game::iIgs036_CmdProcess` (`0x8825920`)
runs once per frame from `Game::Process` and four times in `Game::Initial`:
- `Igs036P2A::SendData`: BAR1 word 0 = byte length, the bytes at BAR0 0,
  ring.  `Igs036P2A::ReadData`: wait for the interrupt flag, then the same
  layout back (at most `0x2000` bytes).
- The interrupt thread (`FPGA_Comm_vInterruptThread`) polls the event
  counter every millisecond and calls back only when it reads exactly 1.
- **Watchdog:** the time from send to reply is kept at `Game+0xA210`; at
  the next exchange, more than 3000 ms makes the game spin forever (`jmp $`
  at `0x8825AA8`).  Message 0 with a non-zero flag clears it.

**Framing** (`Game::iIgs036_AddSendCmd` `0x88248C0`):
- Header `[r][w][w bytes]`, `w = 4·sel + 8` with `sel` from `rand()` and the
  nibble table `m_baBufIndex` (`0x9930F7F`).  The first message is at
  `8 + w`.  For the ASIC's replies the game derives `sel` from `r`, a
  counter and words 2–3; an all-zero 16-byte header gives `sel = 0`.
- Messages `[type][len][data]`; the game pads its data to 4 bytes but parses
  replies by exact length, so the ASIC keeps lengths multiples of 4.
- Trailer: a dword whose low byte is a running byte sum of everything
  before it, carried from message to message in both directions
  (`Game+0xA20C`).  A reply with a bad sum is dropped silently.

**Cipher:** RC4 with one S-box (`Encrypt::_RC4SBox` `0x9E7C640`) for both
directions; `RC4_CypherKey` (`0x8918DF0`) is a standard key schedule and
`RC4_CypherData` (`0x8918E90`) a standard keystream, except that `i` and `j`
restart at 0 on every call.  Each exchange: encrypt the game's message,
then decrypt the reply.  Re-keying by the game's first message:
- **type 0** (start-up): key schedule with `LF KO IGSA36` before encrypting,
  with message 0's payload after;
- **type 5** (whenever a state ends towards 17, the IGS logo): with its
  payload after encrypting.

### Game → ASIC

| Type | Data | Meaning |
| --- | --- | --- |
| 0 | 8–252 B | new channel key (first message of all) |
| 1 | 32 dw | state table: `[k]` = address of the state object for state id `k` (first `Game::Process`) |
| 2 | dw | the current state's status every frame: 1 staying, 2 done |
| 3 | dw | next state id, from each state's `End` |
| 4 | 12 B | IO-board output every frame: byte 0 `0x29`, byte 8 bit 0 coin meter, bit 1 dispenser meter, byte 11 sum of 0..10; the rest lamps |
| 5 | 8–252 B | new channel key |
| 6 | dw | RSA key part request `i` = 0..3 (start-up) |
| 7, 8, 9, 10, 18 | `[0]` / `[1, data]` | read / save: game settings (16 B), sound (12 B), contest (8 B), password (8 B), game lock (28 B); the data XOR-obfuscated with a random key word saved alongside |
| 11, 12, 13, 17 | `[0]` / `[1, data]` | read / save: bookkeeping (1612 B), key-use counts, cards bought, QR rank string |
| 14 | 256 B | the QR reader's text, NUL-terminated, to decode (`CardInput::UpdateScen` `0x86A4FC0`); `**TIMEOUT**` / `**OVERFLOW**` on a reader error |
| 16 | `[0]` | read version |

### ASIC → game (`Game::iIgs036_RecvCmdPrcess` `0x8824AE0`)

| Type | Data | Meaning |
| --- | --- | --- |
| 0 | `[flag][32 B key]` | `Encrypt::AES_vRecvKey` (the `.osmenc` key, handed to the FPGA); flag ≠ 0: NVRAM blank (the first `Game::Process` sets every setting from the defaults and saves), and clears the reply watchdog |
| 2, 3 | dw | **the state object pointer** (`Game+0xA22C`): `Game::Process` runs the state machine (Init / Process / End through its vtable) only through it |
| 4 | 12 B | IO-board input, `r[11]` = sum `r[0..10]`; keys = `r1 \| r2 << 4 \| r3 << 8` |
| 6 | `[i][hex]` | RSA key part: 256 hex digits, 512 for part 3 (`CRSA::_vSetRSAKey` `0x8CDB7C0`) |
| 7 | 32 B | game settings: 16 stored + 16 defaults (checks in `GameSetting::_bRecvSetting` `0x883D110`) |
| 8 | 24 B | sound settings: 12 stored + 12 defaults |
| 9 | 16 B | contest settings: 8 + 8 |
| 10 | 24 B | password: 8 stored + 16 |
| 11 / 12 / 13 | 1612 / 24 / 4 B | bookkeeping, key-use, cards bought |
| 14 | dw | the decoded QR card: its number, printed `%d` with a leading `0` below 10⁸ = the 9-digit code of `QRCardRes.csv` (`QRCard::_vQRCodeDecrypt` `0x881CD50`) |
| 16 | 13 dw | version: low byte of word 2 = region (`0x24` USA → English; 0 China, 2 Indonesia, 4 Japan, 6 Korea, `0x0C` Thailand, `0x0F` Hong Kong, `0x50` other, else Taiwan) |
| 17 | 32 B | QR rank string (`InternetRank::recvQRStr`; an invalid one is reset and saved) |
| 18 | 28 B | game lock (all zero passes its checksum) |

Settings that fail a check show "Data Mistake" and fall back to defaults.
The loader's defaults:
- **Game settings** bytes: 2, 1, 0, 1, 1, 1, then zeros: 1 coin = 1 credit
  (b3/b4), 1 credit per game (b5), no free play (b7).
- **Sound:** 0, 30, 16, 16, 0, 30, 16, 16.
- **Contest:** c3 = 1, the rest 0.
- **Password:** 1 2 3 4.

**The state machine:** state ids (constructors in `Game::Process`): 1 coin
page, 2 how to play, 3 opening, 4 mode select, 5 level select, 6 card
input, 7 loading, 8 decide order, 9 fight, 11 dispenser, 13 game over,
15 operator, 16 demo play, 17 IGS logo, 18 Wahlap logo, 19 game break,
20 break death, 21 warning, 22 editing, 23 test menu, 26 copyright, 27
pass, 28 Hero of Robots mark, 29 select support, 30 ranking, 31 level
select (team).  Boot runs 26 → 21 → 17 → 1; attract cycles 1 → 3 → 30 → 1
(and 17 again, with a re-key).  The ASIC echoes `table[current]`, current
being 26 at boot and then whatever the last message 3 said.

**Assets:** `OsmMgr::LoadOSMFile` (`0x88FBF40`) decrypts
`media/osmbin/*.osmenc` (4288 files) through the FPGA cipher
(`0xC0106610`).  The mask is `4626ed14dc0ee8baed434037f7574619`
(feature `asset_mask`); the key in message 0 does not matter to the
loader's FPGA.

## Peripherals

**Smart card** (`libcasmcard.so` → `libpcsclite` → `pcscd`).  A Java card
in a USB reader holds the operator's points; starting a game needs
`SmartCard::GetSmartCardCredit() > 0` (`Credit::_bGetCoinEvent`
`0x8851100`), and each card bought from the dispenser debits one point.
`SmartCard::Authentication` (`0x8CD6800`), on the card thread:
1. Challenge `[r0 r1 t0..t3]`: two `rand()` bytes and, for a debit, the
   packed local time.  PKCS#1 type 2 padding (`_rsaPack`, `rand() % 255`),
   encrypted with the game's **built-in** 2048-bit public key
   (`CRSA::CRSA`, e = 65537).
2. APDUs to the applet (AID `A0 00 00 00 62 03 01 0C 01 01`): write the
   ciphertext in two halves (`00 0F` query / `00 0D` debit, P1/P2 = offset
   0 / 0x80, 128 bytes), read the answer in two halves (offsets 2/130 for a
   query, 1/129 for a debit).
3. The answer is decrypted with the key the ASIC gave in message 6
   (`CRSA::rsaHPrivateDecrypt`, CRT: parts 0/1 the primes, 2 = p⁻¹ mod q for
   the smaller prime, 3 = d).  Query: `[credit BE16, 0×4, r0, r1, area]`;
   debit: `[credit BE16, r0, r1, area, t, t]`.  The area byte must be the
   region byte from message 16.

**Card dispenser**, `/dev/ttyS1` (`CardDispenser`, 9600 8N1): two-byte
commands `01 xx`, two-byte answers `xx yy`: status `37` → `23` ready; sensor
`38` → `30` or `4B` stock, `31` empty; dispense `42` and reset `40` → `50`
done, `4B` failed.  Other status bytes (`CardDispenser::CheckStatus`):
`16` → 7, `22` → 2, `24` → 3, `25` → −4, `26`/`29` → 6, `27` → 4, `28` → 5.
After a dispense, `CDispenserCardOutProc::CardOut` (`0x86ECB70`) polls the
status every 500 ms and leaves the "cards obtained" screen 2 s after a 7
(`16`) or 8 s after a 6; with `23` it waits forever.  The loader answers
`16` to the two status polls after a dispense (guess: "card taken").  `media/Firmware/C541_HL2_A_140116.Bin` is its firmware
(operator update).  The dispenser holds printed cards: the game never
learns which card came out.

**QR code reader**, `/dev/ttyS0` (`QRCodeReader`, 9600, then 115200): every
command is acknowledged with `06`; a missing reader is state 3 (error),
which blocks starting a game.  Trigger on `1B 31`, off `1B 30`.
`QRCodeReader::ReceiveCode` (`0x8917CD0`) takes `06 <text> 0D 0A` (at most
96 bytes; more than 200 ms between the `06` and the end is a timeout); the
game re-triggers every 3.3 s while it waits for a card.

**QR cards** (`CardInput`, card input state 6): the game does not
understand the printed code.  It sends the reader's text to the ASIC
(message 14), the ASIC answers with the card's number (message 14), and
`CardInput::CheckCard` (`0x86A4E60`) prints it as 9 digits and looks it up
in `media/localization/Card/QRCardRes.csv` (`CardInfo::CheckQRCodeErr`,
`GameInfo::SetPlayerInfo`): kind (`ROBOT`, `POWER`, `SKILL`), QR code,
software id, printed id, name.  The 1002 codes are `0102 SS K NN`: series
`SS` 01–12, kind `K` 1 robot / 2 power / 3 skill, number `NN`.  Boss rows
(`NA…`, `SP…`) cannot come from a number and are refused.  `012900908` is
special (the game's built-in card 7 when a game-info flag is clear).  How
the ASIC decodes the printed text is unknown (its firmware is not dumped),
so real printed cards cannot be scanned.

`/dev/ttyS2..4` are unused; `/dev/input/event*` is only scanned for an OIS
joystick.

## The loader's controller

`games/heroofrobots/`:
- **`heroofrobots.c`**, on the machine's request/response transport
  (`IGS_LOADER.md`):
  - every reply: message 0 (flag 1, see below), the state pointer
    `table[current]`, the inputs (a coin is a one-message pulse);
  - on request: the RSA key parts, version (region `HERO_REGION`, default
    `0x24`), the stored blocks (saves are kept in memory, so every start is
    a first boot: settings come back to the defaults);
  - the serial ports: the QR reader acknowledges everything, the dispenser
    is always ready, stocked and dispenses, and reports the card taken;
  - NFC (below): a tag's card code as the QR reader's text, message 14's
    answer, a dispensed card's code to the tag reader.
- **`libcasmcard.c`** (`build/libcasmcard-hero.so`, SONAME
  `libcasmcard.so`, preloaded): one reader, one Java card with
  `HERO_CARD_CREDIT` points (100), a debit takes one.
  - The real card holds the private half of the game's built-in key; the
    loader cannot decrypt the challenge.  But the challenge and its padding
    come from `rand()` on the card thread: the library interposes `rand()`,
    logs it per thread, rebuilds the padded plaintext from the log and
    **checks it by encrypting it with the game's public key** (it must equal
    the ciphertext written).  No game memory is read and no code patched.
  - The answer is encrypted for real with the loader's own 2048-bit key
    (`card.h`), whose private parts the ASIC plugin hands the game in
    message 6.
- **`nfc.c`**, `nfc.h`: the NFC tag reader, built into the card library
  (which exports `hero_nfc_api`; the ASIC plugin looks it up at run time).

## NFC cards (experimental)

Optional, off by default: NFC tags stand in for the printed QR cards and
for the points card.  Without `HERO_NFC` nothing changes.  Experimental:
tested only with the simulated reader (`IGS_NFC_FAKE`), not yet with an
ACR122U and real NTAG215 tags.

| Variable | Effect |
| --- | --- |
| `HERO_NFC` | `1` or `all`, or a list of `scan`, `dispense`, `points` |
| `HERO_NFC_READER` | PC/SC reader to use: part of its name (default `ACR122`); other readers are never touched |
| `HERO_NFC_WAIT` | seconds a dispensed card waits for a tag (default 30) |
| `HERO_NFC_CARD` | dispense this code instead of a random one |
| `IGS_NFC_FAKE` | a folder: `tag.bin` in it is the tag on the reader (a 540-byte NTAG215 image); no file, no tag |

- **scan:** when the game triggers the QR reader (card input) and a
  loader tag with a card was placed since the last scan, the reader answers
  `06 <code> 0D 0A`; the game forwards the text (message 14) and the ASIC
  answers with the number.  Each placing scans once: lift the tag and put
  it back to scan it again.
- **dispense:** each card the dispenser ejects gets a code
  (`HERO_NFC_CARD`, or random among the 1002 of `QRCardRes.csv`), written to
  the next tag that is blank or the loader's without a card, within
  `HERO_NFC_WAIT` s.  The game's flow is unchanged.
- **points:** the smart card's points live on the tag: each query reads
  them, each debit writes one less.  A blank tag, or a loader tag without
  points, starts with `HERO_CARD_CREDIT`.  With no tag, or another kind of tag, the emulated
  card answers as before.

**Readers:** PC/SC (`winscard.dll`; on Linux `libpcsclite.so.1`, loaded at
run time, so the loader builds and runs without it).  A thread polls every
250 ms; the game never waits for the reader.  Tested with the fake reader
only: a whole game on Windows scanned a robot and a power card, debited
the tag's points and wrote the dispensed card to a blank tag.  Under WSL the ACR122U needs USB passthrough (`usbipd bind`,
`usbipd attach --wsl`, from an admin prompt on Windows), `pcscd` with
`libccid`, and the i386 `libpcsclite1`.

**Tags:** NTAG215 (504 user bytes) or any NFC Forum Type 2 tag whose
capability container (page 3) reads `E1 xx size 00`.  The loader writes
pages 4–17 only, never the UID, lock, capability or configuration pages.
A tag is:
- **blank:** user memory empty (no TLV, an empty NDEF message `03 00`,
  or a terminator `FE`; NULL, lock-control and memory-control TLVs are
  skipped);
- **the loader's:** an NDEF message whose first record is the external type
  `igs-loader:hero`; damaged contents read as no card, no points;
- **other:** anything else, never written.

Page 4 on: `03 32`, record `D4 0F 20 "igs-loader:hero"`, 32-byte payload,
`FE`:

| Offset | Bytes | Payload |
| ---: | ---: | --- |
| 0 | 4 | `HERO` |
| 4 | 1 | version 1 |
| 5 | 1 | flags: 1 card, 2 points |
| 8 | 12 | card code, ASCII, NUL-padded (9 digits) |
| 20 | 2 | points, big-endian |
| 24 | 4 | write count, little-endian |
| 28 | 4 | CRC-32 of bytes 0–27, little-endian |

Every write rewrites the whole record and reads it back.

**Deviation from the cabinet:** the loader's ASIC repeats message 0 (flag 1,
same key) in every reply, not only at start-up.  On a desktop the game can
occasionally take more than 3 s for one exchange (seen once, at the IGS
logo), and the watchdog then hangs it for good; message 0 clears the
watchdog before it is checked.  The flag is otherwise read only once, at
the first `Game::Process`.

## Display and libraries

- Ogre's GL render system on the host's i386 Mesa (llvmpipe), windowed at
  1280×720 as `data/ogre.cfg` says.  The root fs's `libGL.so.1` is
  NVIDIA 195.30 and is never used.
- From the game: Ogre 1.6.1, Cg, zziplib and the NVPerfSDK stub from
  `data/so` (feature `libs`); the render-system and scene plugins load by
  path from `data/so/plugins.cfg`.  From the root fs: `libXfont.so.1`.
  From apt: `libxaw7:i386`, `libfontenc1:i386` (and the usual SDL 1.2,
  `libjpeg62:i386`).
- The loader core hooks `glXSwapBuffers` (overlay, frame cap, `IGS_SHOT`),
  keeps the system Xlib's locks (`IGS_X11_LOCKS=1`: Ogre uses Xlib from
  several threads), and turns OIS's keyboard/pointer grabs, pointer warps
  and global auto-repeat off into no-ops.
- Shaders: most are `arbvp1`/`arbfp1`; a few effects are `vp40`/`fp40`
  only and are missing on llvmpipe (guess: billboards, water).
- **Windows:** Ogre's GLX render system and OIS on the native Xlib/GLX
  (`IGS_LOADER.md`, Windows), on the host GPU.  libXaw, libXt, libSM, libICE,
  libXpm and libXfont are empty stand-ins; the card library is preloaded as
  `libcasmcard-hero.dll`.

## Open

- QR cards: printed codes cannot be decoded (the ASIC's algorithm is
  unknown); without NFC a player always fights with the default robot (the
  card input screen times out).
- NFC on a real ACR122U: untested (fake reader only).
- Settings and bookkeeping are not persisted between runs.
- Player 2 keys and the lever's exact meaning are guesses; the dispenser's
  status byte `16` as "card taken" is a guess that works; the "buy card"
  sub-flow (`CDispenserBuyCardProc`) was not exercised.
- Start-up speed from a Windows drive under WSL (see Status).
- Level-up scene: part of the backdrop turns gray while the animation plays
  (Windows, Intel Iris Xe; not yet tried on NVIDIA).  Suspect: a `vp40`/`fp40`-only
  effect.  Needs a screenshot.
