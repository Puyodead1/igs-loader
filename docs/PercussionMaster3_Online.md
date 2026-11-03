# Percussion Master 3: online

The network side of `pm3` (M107FA), its patch client `pcli` and the
launcher `PowerOn`, enough to write a stand-in server.  Addresses are in
the decrypted `pm3` (dynamic symbols) unless marked `pcli` (stripped) or
`PowerOn`.  Constants are in `IGS_KEYS.md`.  Everything is read from code
unless marked *(inferred)*.

## Summary

- One outgoing TCP connection to the game server, no UDP, no listening
  socket, no cabinet-to-cabinet link: versus is two players on one
  cabinet.  A second program, `pcli`, talks to a patch server and runs
  `wget` (HTTP lists, FTP patches).
- Plaintext: no encryption, no compression.  A length-prefixed frame, a
  `u16` command, a `u32` length and a CRC-16 trailer.
- The store id lives in the IGS036's NVRAM, FPGA-encrypted (messages 14,
  15, 16).  Without a message 15 the game never connects.
- Registration and login answer a challenge with a hex string of
  `a & b`, `a | b`; no keys.
- Login is the 12-key pad only (ID and password digits).  No card
  reader, no QR code.

## Endpoints

| What | Address | Protocol | Source |
| --- | --- | --- | --- |
| Game server | `210.5.22.170:9999` | TCP, the framing below | `rewrite/netGame.cfg`; default hard-coded (`NetManager::NetInit` `0x85B1DF0`); a commented private address in the dump's file |
| Patch server | `210.5.22.170:9998` | TCP, same framing | `rewrite/netPatch.cfg`; default in `pcli` NetInit (`pcli` `0x80594E0`) |
| Patch files | `210.5.22.171:6001` | FTP via `./wget`, user `igs_pm3_patch` | `rewrite/netFTP.cfg` (`pcli` main `0x805ABC0`) |
| Lists | `http://202.153.195.62:8080/{public,pm3}/…` | HTTP via `./wget -i ../rewrite/net.lst` | `rewrite/net.lst` (`pcli` main) |
| Connect test | ICMP echo to `127.0.0.1`, the FTP host, `168.95.1.1`, the game host | ICMP | operator menu, `Setting::ConnectTest` `0x859D630` |

Host names work too: `KoTCP_ConnNonBlock` (`0x860B590`) resolves a
non-dotted name with `net_DNStoIP`.

What rewrites the addresses:
- `net.lst` downloads `store.csv`, `store_list.csv`, `filter_text.dat`,
  `netGame.cfg`, `netPatch.cfg`, `netFTP.cfg` into `rewrite/update/`.
- At boot `PowerOn` copies `update/{netGame,netPatch,netFTP}.cfg`,
  `store.csv`, `filter_text.ini` over `rewrite/` (`PowerOn` main
  `0x804C22A`, copy `0x804A31F`).  The first line of each is a 32-hex
  MD5 of the rest; it is stripped; a mismatch is only logged.
- `pm3` and `pcli` recreate a missing or unreadable cfg from their
  defaults (the old one renamed `.bak`, then `hdparm -f /dev/hdc`,
  `sync`).
- `SettingData` copies `update/store_list.csv` with `cp -uf`.
- Operator menu, network setting 3 (fixed IP, `SettingInfo.dat` +76):
  `killall -9 dhclient`, `ifconfig eth0 <ip> broadcast 255.255.255.255
  netmask <mask>`, `ifconfig lo`, `route add default gw <gw>` from
  `SettingInfo.dat` +0, +4, +8 (`NetManager::iSetIP` `0x85B11C0`, called
  from `Game::iIGS_Init` `0x85E3D20`).

## Configuration files (`rewrite/`)

| File | Content | Read by |
| --- | --- | --- |
| `netGame.cfg` | `NetGame, <host>, <port>` | `_OpenConfig` `0x85B2460` |
| `netFTP.cfg` | `NetFTP, <host>, <port>` | `pm3` (ping only), `pcli` |
| `netPatch.cfg` | `NetPatch, <host>, <port>` | `pcli` |
| `machine.cfg` | `Machine, <generation>, <version>, <edition>, <area>` (dump: 1, 7, 0, 3; `pm3` forces area 3) | both; `pcli` rewrites it |
| `update.cfg` | `Update, <version>, <edition>` (downloaded patch level) | both; `pcli` rewrites it |
| `poweron.cfg` | launcher version | `PowerOn` |
| `update/verNNN/`, `update/edtNNNMMM/` | a patch: `update.lst` (line 1 a Unix time to apply after, then `<op>, <path>, <MD5>`; `r` = replace) and the files | `PowerOn`; `NetManager::NetUpDateCheckStart` `0x85B0FB0` shows "updating" |
| `store.csv`, `store_list.csv` | MD5 line, then province / region / store ids and names (CSV, Big5) | operator menu store choice |
| `filter_text.dat`, `.ini` | MD5 line, then banned words (nicknames) | sign-up |
| `SettingInfo.dat` | +0 IP, +4 mask, +8 gateway, +12 MAC (the connect test saves it), +18 country, +19 province, +20 `u16` region, +24 `u32` store, +28 `char[24]` store name, +57 coins per credit, +74 game level (0x138), +75 store changed, +76 IP mode | `SettingData` |

`./checkUpdate` (a shell script: `touch`/`rm ../rewrite/updating`
depending on whether `wget` runs) is run at every screen change
(`NetManager::NetCheckUpDate` `0x85B1180`).

## Transport

`KoTCP_*` (`KoTCP_SendMsg` `0x860C930`, `KoTCP_RecvMsg` `0x860CCE0`),
`MessageSend` `0x8612AF0`, `MessageRecv` `0x8612A50`:

| Bytes | Field |
| --- | --- |
| 0 | `u16 L` = n + 8 (1..4096) |
| 2 | `u16` command |
| 4 | `u32 n`, the data length |
| 8 | data |
| 8 + n | `u16` CRC-16/ARC of the data |

- Little-endian, strings fixed-length and zero-padded.  Nothing is
  encrypted or compressed; one frame per message, no fragments.
- `L` = `0x4BFF` alone is a ping, answered `0x4FFF`.  Either side pings
  after 10 s without data; 60 s without data drops the link (`NetSetKeepalive(10)`,
  `NetSetTimeout(60)`).  Any other `L` above 4096, or 0, drops it.
- A bad CRC drops the message (`-7`).
- `NetManager::NetRecv` `0x85B1840` takes one message per frame; the
  data goes to a decoder (`_GetRecvMessage` `0x85B2760`) and the screens
  see only the command id of that frame.  Builders are in
  `_SetSendMessage` `0x85B3E10`.  Bursts are fine; each is seen in turn.
- Odd ids go to the server, even ids come back; `pcli` logs them as
  three nibbles (group, item, step).

## The IGS036 and the store id

The game keeps no registration on disk.  `Game::iIGS_Init` `0x85E3D20`:

1. Sends chip message 14 (a random word): the stored network data is
   wanted.
2. The chip answers message 15, 16 bytes
   (`NetManager::iRecvNetDataFrom36WithDecrypt` `0x85B1510`).  Word 0 zero:
   no registration.  Else the 16 bytes are decrypted by the FPGA cipher
   with the key at `0x989E1E0`: `{u32 store id, MAC[6], 0}`.
3. Only then does `NetManager::NetProcess` `0x85B19F0` leave state 1.  A
   non-zero MAC that differs from `eth0`'s stops it for good (state 2).
4. Waits up to 6 s (60 × 100 ms) for the time (0x134), then starts the
   game.

After a registration reply (0x114) with a store id, and after the
operator menu's reset items (`Setting::MainMenu` `0x85A0200`), the game
FPGA-encrypts `{store id, MAC, 0}` and sends it as chip message 16
(`NetManager::pubResetNetDataWithEncrypt` `0x85B13C0`, from
`Game::iNetUpData` `0x85E36B0`); the chip keeps it for the next boot.

The loader's plugin answers both: message 16 is kept in
`rewrite/igs036.nv` and given back as message 15 (16 zero bytes before
the first registration).  The real FPGA mask of this key is unknown (no
cabinet's blob was dumped); encrypt and decrypt both run on the emulated
FPGA, so the plugin gives the key its own mask (all zero), decrypts the
blob and clears the MAC before keeping it: a zero MAC is never compared,
and eth0's differs between the loaders.

## Connection

`NetManager::NetProcess` `0x85B19F0`, run every frame from
`Game::iNetUpData`:

| State | |
| --- | --- |
| 1 | wait for chip message 15 |
| 2 | MAC mismatch, stays |
| 3 | `KoTCP_ConnNonBlock(host, port)`; retry after 600 frames, after 3 failures 18000 frames (5 min) |
| 4 | wait for the connect; then keepalive 10 s, timeout 60 s |
| 5 | store id ≤ 0: send 0x111, else 0x121 |
| 6 | 0x112 → 0x113; 0x114 with id > 0 → 5 (log in); 0x122 → 0x123; 0x124 ≥ 0 → 0x411, state 7; < 0 → 5 |
| 7 | 0x412 → 0x413; online from here (`NetManager::CheckConnect`: state > 6) |

A dropped link (send or receive error, `NetDisconnect`) goes back to 3.
Every screen change reconnects at once if offline
(`NetManager::NetReConnectNow`, `Game::Process` `0x85E4A80`).

**Registration** (store id 0):

| Cmd | Dir | Data |
| --- | --- | --- |
| 0x111 | → | 8: `u16` generation, `u16` version, `u16` edition, `u16` area (`machine.cfg`: 1, 7, 0, 3) |
| 0x112 | ← | 8: `u32 a`, `u32 b` (challenge) |
| 0x113 | → | 38: `char[16]` = `sprintf("%08X%08X", a & b, a \| b)` (no NUL), 16 bytes CPUID leaf 1 (`eax`, 0, `ecx`, `edx`; `Ko_GetCPUID` `0x86047B0`), `eth0` MAC[6] |
| 0x114 | ← | 4: `u32` store id (≤ 0: stay; > 0: stored in the chip, then log in) |

**Login:**

| Cmd | Dir | Data |
| --- | --- | --- |
| 0x121 | → | 12: `u32` store id, then 0x111's 8 bytes |
| 0x122 | ← | 8: `u32 a`, `u32 b` |
| 0x123 | → | 38: as 0x113 |
| 0x124 | ← | 1: `s8` result (≥ 0 accepted) |
| 0x411 | → | 0 |
| 0x412 | ← | 257: `u8 n`, song ids (the "hot songs" list, select type 4, `SelsongList::ChangeListType` `0x85B8A30`): `u8 ids[256]` |
| 0x413 | → | 0 |

**Time:** 0x134 (8: `u32` seconds, `u32` microseconds) is applied only
during start-up (init state ≤ 4): `settimeofday`, `system("hwclock
--systohc")`, timers reset.  Nothing asks for it: the server pushes it
after login.

**Versions sent:** generation, version, edition, area (0x111, 0x121,
0x311); no executable or chip version.  No HMAC or session key.

## Commands

Player-addressed messages start with `u8 p` (0 or 1, `NetData+180`).
"Unsent" means a builder exists but no caller was found.

**Machine:**

| Cmd | Dir | Size | Data | Use |
| --- | --- | --- | --- | --- |
| 0x131 | ← | 25 | +0 province, +2 `u16` region, +4 `u32` store, +8 `char[16]` store name, +24 coins per credit | store info from the server *(inferred)*; a 28-byte builder of the same, unsent |
| 0x132 | → | 32 | a day's bookkeeping (below) | `SettingData::vNetSendDayIncome` `0x85AE6B0` |
| 0x133 | → | 112 | a month's bookkeeping (below) | `vNetSendMonthIncome` `0x85AE540` |
| 0x134 | ← | 8 | time | above |
| 0x135 | → | 0 | operator restored the default settings | `Setting::RestoreOriginalSetting` |
| 0x136 | → | 0 | operator deleted the earnings record | `Setting::DeleteEarningRecord` |
| 0x137 | ← | 2 | `u16`, stored, never read | |
| 0x138 | ← | 1 | game level: written to `SettingInfo.dat` +74 and `GameInfo` `m_GameLevel` | `Game::iNetUpData` |
| 0x139 | ↔ | 36 | +0 country, +1 province, +2 `u16` region, +4 `u32` store, +8 `char[24]` store name, +32 coins per credit | sent when the operator changes the store or `store_list.csv` changes (`Coinpage::Init` `0x85E2BC0`, `Setting::Exit` `0x858A9E0`); received: overwrites it |

Bookkeeping (from `SettingInfo` day/month records, 30 days, 12 months;
all marked at boot, one sent per frame outside the song and the operator
menu, `SettingData::vNetCheckSendRecord` `0x85AE7E0`):

| Off | 0x132 / 0x133 |
| --- | --- |
| 0 | `u16` year |
| 2, 3 | `u8` month, day |
| 4 | `u16` record +14 (the divisor below; plays *(inferred)*) |
| 6, 8, 10, 12, 14 | `u16` record +12, +16, +18, +20, +22 |
| 16 | `u32` record +24 |
| 20 | `u32` record +24 − record +28 |
| 24 | `u16` record +28 / record +14 |
| 28 | (0x133) 4 × 5 `u32` tables, to 112 bytes |

**Player and account:**

| Cmd | Dir | Size | Data | Use |
| --- | --- | --- | --- | --- |
| 0x211 | → | 17 | `p`, `char[16]` new ID | sign-up: is the ID free? (`Login::myNetCmd` `0x85A2CD0`) |
| 0x212 | ← | 2 | `p`, `s8` result: 0 free (the server keeps the ID for this player), −2/−5 taken | `Login::NetUpdata` `0x85A5DB0` |
| 0x213 | → | 34 | `p`, `char[16]` password, `char[16]` nickname (Big5, pad keyed in), `u8` role | sign-up done (`Sign::NetCommand` `0x85873C0`) |
| 0x214 | ← | 2 | `p`, `s8` result (0 ok) | `Sign::NetUpdate` `0x8587060` |
| 0x215 | → | 33 | `p`, `char[16]` ID, `char[16]` password | member login |
| 0x216 | ← | 2 | `p`, `s8` error: −4 / −2 wrong ID or password (asked again once), −5 "this account has been locked", −6 "this account is already logged in" (both seen in the game), other: network error | |
| 0x217 | ← | 2004 | profile, 150-song layout | login / sign-up success; 2004-byte builder unsent |
| 0x218 | → | 1 | `p` | logout (`GameData::PlayerLogout` `0x85FF1F0`) |
| 0x219 | → | 1 | `p` | a sign-up left half-done (ID reserved, no nickname): from `Sign` at its time-out, and for each such player when the operator menu opens (`Setting::Init` `0x858E9C0`) |
| 0x21A | → | 1 | `p` | continue (`GameData::PlayerNetContinue` `0x85FE340`) |
| 0x21B | ↔ | 2748 | profile, 256-song layout | in: as 0x217; out: save (`GameInfo::vSendPlayerDataToNet` `0x85FD3B0`) |
| 0x221 | → | 2 | `p`, mode byte | unsent |
| 0x222 | → | 2 | `p`, the difficulty chosen (profile byte 1933) | `SelLevel::Process`, `GameData::RestoreModeState` `0x85FE3A0` |
| 0x223 | → | 2 | `p`, last song for that choice | unsent |
| 0x231 | → | 144 | play log (below) | `GameInfo::vSendPlayerLog` `0x85FCF80` |

**Rankings** (screen `netrank`, id 33, `netrank::NetUpdata` `0x85F6210`;
contest `InfoMatch`, id 32, `0x85B60D0`).  Kinds on the wire
(`netrank::DataInit` `0x85F7C20` maps its board index 0..4 to kinds 0, 1,
3, 4, 2): 0 normal and 1 battle drum mode (`m_mode` 1, the local
`RankList_Nostop_*`), per difficulty; 2 the Knights' song contest (board
13, best score on its song), 3 the score contest (board 11, total), 4 the
combo contest (board 12, best combo).

| Cmd | Dir | Size | Data |
| --- | --- | --- | --- |
| 0x232 | → | 20 | `p`, kind, `u8` level choice, `u8` title; kinds 0, 1, 3, 4: +12 `u32` total score, +16 `u16` best max combo, +18 `u8` stage count; kind 2 (contest 13): +4 `u8` class, +5 `u8` song, +8 `u32` best score on the contest song (`GameInfo::vSendRankingData` `0x85FD970`) |
| 0x233 | ← | 14 | `p`, kind, `u16` place before this upload, the best before it: kinds 0, 1, 3 +8 `u32`, 2 +4 `u32`, 4 +12 `u16` ("new record" when this game's value reaches it, `netrank::DataSet`) |
| 0x234 | → | 2 | `p`, kind: the place now |
| 0x235 | ← | 4 | `p`, kind, `u16` place (the rows shown start two above it) |
| 0x241, 0x243 / 0x245..0x249 (odd) | → | 4 / 2 | class, pad, `u16` start / `u16` start; no caller: an older form of 0x291..0x299 |
| 0x242, 0x244 | ← | 304 | `u8` class, pad, `u16` start, 5 × 60-byte entries |
| 0x246, 0x248, 0x24A | ← | 344 | `u16` start, pad, 5 × 68-byte entries |
| 0x251, 0x253 | → | 6 | `u8` class, pad, `u16` start (50 × page), `u16` count (50); kinds 0, 1 |
| 0x255, 0x257, 0x259 | → | 4 | `u16` start, `u16` count; kinds 4, 2, 3 |
| 0x252, 0x254 | ← | 6 + 16 n | `u8` class, pad, `u16` start, `u16` n, n × `char[16]` (entry +28) |
| 0x256, 0x258, 0x25A | ← | 4 + 16 n | `u16` start, `u16` n, n × `char[16]` |
| 0x291, 0x293 | → | 4 | `u8` class, pad, `u16` start (rank − 3, at least 0); kinds 0, 1 |
| 0x295, 0x297, 0x299 | → | 2 | `u16` start; kinds 4, 2, 3 |
| 0x292, 0x294 | ← | 344 | `u8` class, pad, `u16` start, 5 × 68-byte entries |
| 0x296, 0x298, 0x29A | ← | 344 | `u16` start, pad, 5 × 68-byte entries |

Ranking entry, 68 bytes (tables of 500 per class at `NetData`
+11120..; 0x29x write each wire entry at table +4, 0x246..0x24A at +0;
read by `netrank::DataSet` `0x85F8110`), table offsets: +12 country
(`store_list.csv`, else "OTHER"), +16 `char[16]` store name, +40
`char[16]` nickname (also the 200-place tower, all 0x25x fill), +56
title, +60 `u32` the value shown (+64 `u16` for kind 4).  The 60-byte
entries (0x242, 0x244) put +13, +14, +16, +40, +56, +60, +64 at 12, 14,
16, 32, 48, 52, 56.  The
requests carry no player byte; the replies fill the player's table.
Flow on the screen: 0x232
(if the player has a member id), 0x234 → 0x235, 0x29x → 0x29x+1, then
0x25x → 0x25x+1; each step retried after 120 frames, up to 10 times.

**Boards, ticker, dragon:**

| Cmd | Dir | Size | Data | Use |
| --- | --- | --- | --- | --- |
| 0x271 | → | 0 | boards wanted | `DemoData::DemoNetRequest` `0x85FE2B0`, once when online |
| 0x272 | ← | 920 | 2 × 460-byte board (below) | `BoardInfo::SetBoardDataFromNet` `0x86034C0` |
| 0x273 | → | 0 | ticker wanted | same |
| 0x274 | ← | 1088 | 4 × 272-byte ticker: the board's first 270 bytes | `RunWordInfo::SetRunWordDataFromNet` `0x8553800` |
| 0x416 | ← | 1 + n | `u8 n`, n bytes notice text (≤ 256) | coin page notice (`DemoData::SetNoticeStringFromNet` `0x85FCF20`); never asked for |
| 0x281 | → | 1 | `p` | dragon list (`InfoDragon::Process` `0x85A2110`, screen 30) |
| 0x282 | ← | 604 | `p`, pad, 5 × 10 × {`u32`, `u8` grade, pad, `u32`}; a group with more than 6 grade-8 entries counts as won | `GameInfo::vGetPlayerDragonFromNet` `0x85FE1C0` |
| 0x283 / 0x286 / 0x289 | → | 2 / 1 / 2 | `p`, `u8` dragon index (0..2) / index only / `p`, index | `SelSong::NetUpdata` `0x85B8400` |
| 0x284 / 0x287 / 0x28A | ← | 84 | `p`, index (0x287: index), pad, 80-byte dragon | to `NetData`+9676 + 80 × index |
| 0x285, 0x28B | → | 8 | `p`, index, pad, `u32` damage *(inferred)*; 0x28B when the dragon is kind 9 | `Stage::CheckDragonPlay` `0x857F750` |
| 0x288 | → | 8 | index, pad, `u32` (single-player variant) | same |
| 0x28C | → | 1 | index: drop a dragon | `SelSong::NetUpdata` |
| 0x2A1 | → | 2 | `p`, `u8` page (0..4) | dragon 2 board (screen 35) |
| 0x2A2 | ← | 208 | `p`, page, pad, 204 bytes | to `NetData`+9920 + 204 × page |
| 0x2A3 | → | 2 | `p`, 2 | dragon 2 wanted |
| 0x2A4 | ← | 72 | `p`, pad, 68 bytes | `NetData`+10940 |
| 0x2A5 | → | 8 | `p`, pad, `u32` | dragon 2 result |
| 0x2A6 | → | 1 | index | drop a dragon 2 |
| 0x2A7 | → | 3 | `p`, index, `u8` | dragon 2 entry |

Board, 460 bytes (`BoardInfo`, `RunWordInfo::UpdateRunWordData`
`0x85533D0`): +4 `u8` kind, +6 `u16` year, +8 month, +9 day, +10 `u16`,
+12, +13 (the period; `RunWordData::IsConformDate`), +14 `char[256]`
text, +270..272 three bytes (the samples: 1, 2, 0; never read), +276 10 ×
{`u8`, `u8`, pad, `u32` value}, +356 10 × {`u32` level, `u32`}, +436
`u8[10]` songs (kinds 19, 21, 24; 0xFF ends) or kind 13's song, +448
`u32`, +452 `u32` (copied, never read).  The game runs the events from
these (`House::CalcPlayerData` `0x85678E0`, `CheckLevelUp` `0x8567680`):
18 gives value[0] EXP to a new member's first game, 15..17 and 21
multiply the EXP of their songs by value[0] % (at most 300), 20 gives
value[i] EXP on reaching level[i].

Kinds, from the game's own sample boards and tickers
(`BoardInfo::SetFakeData` `0x8602E70`, texts at `0x9540540`, `0x95402AC`;
`RunWordInfo::SetFakeData`, `0x9529C80` + 128 × (kind + 2)): 1..10
holidays (New Year, Labour Day, Dragon Boat, Valentine's, Mid-Autumn,
National Day, Singles' Day, Christmas, Children's Day, Women's Day: a
holiday cursor in the songs, hits counted), 11 score contest ("Drum
Spirit Battle", battle drum mode), 12 combo contest, 13 the Knights' song
contest (a set song), 14 dragons, 15 / 16 / 17 classical / pop / original
song festival, 18 sign-up bonus, 19 new songs, 20 level rewards, 21 song
festival, 22 dragon 2, 23 / 24 PIU / ULTRAWAVE songs.  The newest running
holiday, contest, dragon, dragon 2 and other board each fill one slot.
Event titles (fields 25..38 of `pyTitle`, "Event reward") and the event
songs (150..158) and cursors are not given by the game (`hurule.lua`
`event=0`): the server grants them in the profile.

Dragon, 80 bytes (`GameInfo::vGetDragonInfoFromNet` `0x85FFA30`): +0
`u32` present, +5 `u8` kind (0..8 pick its song 141..149,
`InsertInfo::SetInsertInfo` `0x8563980`; 9: a 20-point dragon, else 100),
+8 `u32` points left, +24 `u8` province (`store_list.csv`), +44
`char[16]` name, +60 `char[16]` finder.  `SelSong::InitDragonData`
`0x85B8320` hides each dragon behind a random song of the list (a dragon
icon, `UpdateList`); a player who picks it fights it after the stage
(0x285; the catch score depends on the difficulty, 1200..3000), the others
are dropped (0x28C).  0x282 grades: 1..3 seen, 5..7 fought, 8 caught, 9
lost; a difficulty with more than six 8s is complete.  Dragon 2 (0x2A2,
0x2A4): +0 present, +5 slot / kind, +6 grade, +20 country, +48 name (the
catcher on 0x2A2: "%s from %s" with +64 the time).  The world dragon is
rolled per stage in song select (20 / 40 / 60 %).

## Player profile

0x21B (2748 bytes, both ways); `GameInfo::vGetPlayerDataFromNet`
`0x85FDBC0` copies it into the Lua `Player` (`Bind_vRegister`
`0x85BEB20`):

| Off | Size | Field |
| --- | --- | --- |
| 0 | 1 | `p` |
| 4 | 4 | member id (`Player`+24, the Lua's commented `m_netId`); > 0 is what rankings need |
| 8 | 16 | nickname (13 bytes used) |
| 24 | 1 | `m_role` |
| 25 | 1 | `m_lv` |
| 26 | 2 | `m_exp` |
| 28 | 2 | `m_versus` |
| 30 | 2 | `m_win` |
| 32 | 2 | `m_maxCombo` |
| 34 | 5 × 256 | grade per class and song; 7 and 8 count into `m_bravoNum` |
| 1314 | 256 | songs unlocked (`m_getSong`, via `GameConfig`'s table) |
| 1570 | 256 | charts unlocked (`m_getBand`) |
| 1826 | 40 | `s8` titles (`m_getTitle`, −1 none) |
| 1866 | 3 | Speed, Hide, Sudden options unlocked, 0..5 (`Player`+148, +172, +196; `common.lua` `optionReward`) |
| 1869 | 16 | cursors unlocked (`Player`+220; `optionReward` [2]) |
| 1885 | 12 | `s8` drum skill levels (`m_getSkillLv`) |
| 1897 | 12 | drum skill experience (`m_getSkillExp`) |
| 1912 | 4 × 4 | `m_100ComboCnt`, `m_contCnt`, `m_heavyCnt`, `m_TotalHolidayCursor` |
| 1928 | 5 | `u8` × 5 (`Player`+7488..7504, no Lua name; kept as sent) |
| 1933 | 1 | the difficulty chosen (as 0x222) |
| 1934 | 5 | last song per difficulty (`m_selSong` goes to [choice]) |
| 1939 | 1 | `m_title`, the title worn |
| 1940 | 7 | options in use: Speed (`Player`+56), Hide (+60), 0, cursor (+64), `s8` skill (+72, −1 none), Sudden (+68), a flag (out only) |
| 1947 | 1 | n ≤ 100 |
| 1948 | 800 | n × 8-byte items (`Player`+5872) |

0x217 (2004 bytes) is the same with 150-entry arrays: grades at 34
(5 × 150), songs 784, charts 934, titles 1084, then everything 742 bytes
earlier (n at 1203, items at 1204).  On receipt the player is marked
logged in, `m_isGuest` cleared, `m_contWin`, `m_skillLv`,
`m_HolidayCursor` zeroed, `m_selSong` −1.

**What song select needs:** a 0x217 or 0x21B for that player.  An
all-zero profile with a nickname, a member id and level 1 is enough;
locks follow the unlock arrays.

## Accounts and login

`Login` (screen 4, `Login::Confirm` `0x85A3CD0`, `Login::MsgProc`
`0x85A4F60`): guest, member or new, on each player's pad; every network
choice needs the link up.

- **Member:** ID, then password (digits, `char[16]`) → 0x215 → 0x21B
  (or 0x217) → in.  0x216 shows the error; a first wrong password asks
  again, any other error continues as a guest.
- **New:** ID → 0x211 → 0x212 0 → password twice → screen `Sign`
  (6): nickname from the phonetic keyboard, role → 0x213 → 0x21B / 0x214.
  An unfinished sign-up sends 0x219.
- **Guest:** nothing is sent.

## After a game

`House::NetUpPlayerData` `0x85671C0`, per logged-in player:
1. 0x21B, the profile (above).
2. 0x231, the play log:

| Off | Data |
| --- | --- |
| 0 | `p` |
| 1 | mode, from `m_VSType` / `m_mode`: VS type 0: 0 (1 when `m_mode` 1); 1: 2 (4); 2: 3 |
| 2, 3 | `u8`, `u8` (`Player` values; 3 keeps the maximum) |
| 4 | `u32` the other player's member id |
| 8 | `u8`; +10 `u16`, +12 `u8` (not filled) |
| 13 | `m_stageCount` (stages 0..n follow) |
| 16 | up to 6 × 20-byte stages: `u8` class, `u8` song, pad, `u32` final score, `u16` max combo, `u8` HP, `u8` grade (8, 7 for the two full-combo flags, else `m_enRank`), `u8` passed, `u8` speed, hide, sudden, cursor, skill, pad 2 |
| 136 | `u32` total score |
| 140 | `u16` best max combo |
| 142 | `u8` VS type 2: 1 when more than one stage was won |

3. 0x232 for rankings of kind 0 or 1 (by mode), or the running contest.

Nothing else is uploaded: the camera photos stay in `rewrite/photo`
(`Photo::SavePhoto`), local rankings and top scores in `rewrite/*.dat`.

Downloads: 0x272 boards (holiday, contest, dragon, other), 0x274 the
ticker, 0x416 the notice, 0x412 the hot songs, rankings on demand, and
by `pcli` patches and the lists.  No song lists or charts come over the
game link; new songs come as patches (`script_download`, ROMs).

## Multiplayer, events

- **Versus** is two players on one cabinet; the server only gets both
  logs (opponent id in 0x231).  No matchmaking or in-song sync exists:
  no UDP (the `KoUDP_*` functions are unreferenced), no listening
  socket.  `Match` (28) shows boards in the attract loop.
- **Contest** (`InfoMatch`, 32): board kinds 11..13; 0x232 → 0x233 →
  0x234 → 0x235, then a 20 s ranking page.
- **Dragon** (`InfoDragon`, 30; dragon 2, 35): shared targets, up to 3
  (5 pages for dragon 2) fetched in song select; a dragon stage reports
  0x285 / 0x28B / 0x288 / 0x2A5.
- **Holiday:** board kinds 1..10; cursors in the profile.

## What needs the server

- Member accounts, the profile, unlocks and titles kept between games.
- The "hot songs" list (offline: a built-in list), boards, ticker and
  notice, dragon and contest screens, net rankings, the "online" mark.
- Patches (a separate server).
- Time setting at boot.
- Offline: guests only, local rankings and photos; all songs not locked
  by profile are playable.

## Patch client (`pcli`)

Same framing; `NET_IDLE` loop in `pcli` main `0x805ABC0`:

| Cmd | Dir | Data |
| --- | --- | --- |
| 0x311 | → | 8: generation, version, edition, area |
| 0x312 | ← | 8: `u32 a`, `u32 b` |
| 0x313 | → | 38: as 0x113 |
| 0x331 | ← | 4: `u16` version, `u16` edition (newest) |
| 0x332 | → | 4: `update.cfg` version, edition (when older) |
| 0x333 | ← | 1: `s8` error |
| 0x334 | ← | 4: the version to fetch; `pcli` runs `wget … ftp://user:pass@<ftp>/pm3/patch[_fa]/verNNN/` (`patch_fa` for area 3) and `edtNNNMMM/`, checks `update.lst`, writes `update.cfg` |
| 0x336 | → | 264: generation, `u16` version, `u16` edition, area, `char[256]` status text |
| 0x337 | ← | 4: delete that update directory → 0x336, 0x338 |
| 0x339 | ← | 4: set `machine.cfg` / `update.cfg` to it → 0x336, 0x33A |
| 0x338, 0x33A | → | 4: version, edition (done) |

Connect retries: 3 × 10 s, then 300 s; when current, it checks again
after an hour.  `PowerOn` applies a patch at the next boot.  The loader
does not run `pcli` or `PowerOn`.

## Resolved

- No caller: 0x131 (out; the 0x139 path does the same), 0x217 (out),
  0x221, 0x223, 0x241..0x249 (an older ranking-page form).  Handled but
  never asked for: 0x131, 0x137 (a word stored, never read), 0x139 (in:
  replaces the store info), 0x242..0x24A, 0x416 (the coin page notice):
  the server pushes 0x416 after login.
- 0x216 −6, 0x219, 0x222, 0x234, the ranking kinds and entries, the
  boards' parameters, the dragon payloads, the profile bytes, 0x412's
  last byte: above.
- The FPGA mask of the network-data key: unknown on a real FPGA, not
  needed (above).

## Screen flow online

The IGS036 decides the screens (`IGS_E2200.md`); online the plugin adds,
from the results the screens report:

| After | Next |
| --- | --- |
| Coinpage | Login; result 1 (`GameData` +19716) = online |
| Login | Sign when result 0 / 1 = 1 (that player finished a new ID's passwords, `Login::Confirm`); result 4 = 1: InfoDragon 35 (dragon 2, level 10+); else Match when online, SelType offline |
| Sign | Sign again when result 4 % 16 = 1 (the other player signs up too); 3: InfoDragon 35; else as Login |
| Match | in a game (result 3 % 3 = 1): SelType; in the attract: Logo |
| House | 1: the second player's House; 3 / 4: InfoMatch / InfoDragon, then that House |
| Rank | netrank when result 2 & 1 (online) |
| netrank | Thank after a game, Match in the attract |

## Stand-in server

igs-online's `pm3` (`docs/PM3.md` there).  The loaders route only
the game's connection (port 9999, the list's `IGS_NET_PORTS`) to
`IGS_NET`; `pcli`, `PowerOn`, `wget`, the HTTP lists and the pings never
run or are refused, so nothing re-points the game or reaches a real
host.
## Compared with We Dance's KoNet

Shared: the IGS `Ko` library (`KoTCP_*`, `hash_CRC16`, `aes_*`,
`des_*`, `zlib_*`, `icmp_*`): the `u16` length prefix, `0x4BFF` /
`0x4FFF` keepalive, 10 s / 60 s, CRC-16/ARC, odd-request even-reply ids
in groups (0x1xx machine, 0x2xx player, 0x13x bookkeeping), MD5-headed
downloaded lists, `hwclock` after the server's time.

Different in PM3:

| | We Dance (KoNet) | PM3 |
| --- | --- | --- |
| Layer | `KoNetAP` | `MessageSend` / `MessageRecv`, no `KoNetAP` |
| Header | `u16 cmd & 0xFFF \| mode << 12 \| 0x4000` | `u16` cmd, `u32` length, CRC trailer |
| Size | 18000 per frame, 6 MB fragmented | 4096, no fragments |
| Crypto | static AES-256, session AES-128, HMAC-MD5 | none; `a & b`, `a \| b` hex reply |
| Compression | zlib modes | none |
| Ids | login 0x11..0x14, registration 0x21..0x24, player 0x211.. | registration 0x111..0x114, login 0x121..0x124, then 0x411..0x413; player 0x211.. with other payloads |
| Store id | the launcher's shared memory (IGS027A type 18) | the IGS036 NVRAM, FPGA-encrypted |
| Server lookup | HTTP `rf.igs.com.tw` | `net.lst` over HTTP rewrites the cfgs (via `PowerOn`) |
| Login | keypad, QR via phone app | keypad only |
| Versus | over the server | local only |
| Patches | `wedoCliPatch.exe` | `pcli` + `wget` FTP |

The player ids overlap (0x211 login, 0x213, 0x215, 0x219, 0x21B) but no
payload matches; a We Dance server cannot be reused, only its transport
shape.
