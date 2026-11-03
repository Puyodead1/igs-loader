# We Dance Online 2 (E2000)

唯舞独尊2, on screen "We Dancing Online Plus" V2.34.  Product string
`WeDo-e2000-v1-e64`, program `M212CN` (`system/CFVER`), patch level 56.  A
networked rhythm game: arrow notes scroll to a judge ring and are hit on a
console of arrow buttons and OK; a phone keypad (1..9, *, 0, #) enters
accounts and names.  Online: member accounts (card reader, QR login),
rankings, a market, contests, a patch server.

**Status:**
- Fully extracted from the stock image; the executable is UPX-packed and
  unpacked statically (`unupx`).
- **Plays in the loader** (machine `e2000`, plugin `wedanceonline2`), both
  loaders: warning, attract (logo and opening videos, how to play, demo
  dances) with sound, coins, title, offline song select, songs with
  judgements, combos and score, the song result, the next songs, game
  over, back to the attract; the operator menu on F2 (2026-11-01).
- The IGS027A is the I/O board and settings store, with Go Go Ball 2's
  packet transport; the game logic is in the executable (`IGS_E2000.md`,
  We Dance).
- **Offline** by default: no IP network (list `env` `IGS_NO_NET`), so the
  game finds no server and runs its own offline mode: guest play,
  "OFFLINE" on the song select.
- **Online** with `IGS_NET` and the igs-online server (below): members,
  saved progress, account transfer, QR login, the market and lucky bags,
  missions, groups, the world tour, rankings, versus and co-op between two
  instances (2026-11-02).

## Playing

```powershell
.\play.ps1 wedanceonline2        # WSL
.\play-win.ps1 wedanceonline2    # native Windows
```

The warning screens come after about 30 s (Windows) to 80 s (WSL, set on a
Windows drive).  A coin and OK start a game; a credit plays several songs.

| Key | Cabinet |
| --- | --- |
| 5 | coin |
| Enter, Space | OK |
| Arrows | arrow buttons |
| 1 2 3 / Q W E / A S D / Z X C | keypad 1 2 3 / 4 5 6 / 7 8 9 / * 0 # |
| Numeric keypad | the digits as labelled; / `*`, * `#` |
| 0 1 2 3 4 6 7 8 | the same digits (5 and 9 are coin and service) |
| 9 | service (a credit) |
| F2 | test (operator menu) |
| F1 | show / hide the key help |

- **The game's own key:** `/` shows the warm-up timer.
- **Operator menu:** Up / Down move, OK enters; its key test shows the
  console.
- **Settings and bookkeeping:** the coin settings and password are the
  IGS027A's, in `info/igs027a.nv`; the rest are the game's own files in
  `info/` (`gameset.bin`, `bookEnc.bin`, `soundset.bin`, ...), all in
  `/WeDo/linux` of the rom set (for a `.7z` set, the cache's
  `<cache>/<linux|windows>/wedanceonline2/WeDo/linux/info/`).
- `WEDO_REGION=n` sets the IGS027A's region: 1 China (default; Simplified
  Chinese), 2 Hong Kong, 3 other, 4 USA, 5 Korea, 6 Thailand (4..6
  English), other Taiwan.
- `WEDO_SCRIPT="sec:key[+],..."` presses keys on a timetable for testing,
  from the first frame.  Keys `coin a up down left right arrows service
  test 1..9 star 0 hash` (`arrows`: all four at once); a press lasts 0.2 s,
  or with `+` until the next entry.  `WEDO_SCRIPT_CLOCK=frames` times it in
  game frames (60 a second) instead of wall time.
- **Key presses:** every press is reported to the game for 4 frames at
  least, and repeats of a key get a released frame in between, so short
  taps and fast repeats (keypad digits) all count.
- **"Button malfunction" (按钮故障) on the coin page:** the game's own
  stuck-button test.  Each credit adds one to OK's and each arrow's count
  in `info/keyerror.dat`; a press seen in 3 frames in a row clears it; 4
  credits without one cross the button out.  Press each crossed-out button
  once to go on.  Before the 4-frame reporting above, keyboard taps shorter
  than 3 frames were not counted.

## Online play (experimental)

The igs-online server (a separate project, Python 3) stands in for IGS's
servers.

```powershell
python -m igs_online wedance                       # in the igs-online folder
$env:IGS_NET = "127.0.0.1:10000"; .\play-win.ps1 wedanceonline2
```

In WSL: `IGS_NET=127.0.0.1:10000 ./run.sh wedanceonline2` (a server run in
WSL; for one on Windows use its address).  `IGS_NET` routes only the game
server's connection (port 10000, `IGS_NET_PORTS`); the web lookups, banners,
pings and patch server are refused, so `info/ip` is never rewritten.

- **First connection:** the cabinet registers (0x21..0x24) and keeps the
  store id the server gives it in `info/igs027a.nv`.  A fresh server
  database does not know it: delete `info/igs027a.nv` (coin settings and
  operator password too) to register again.
- **Login screen** (after a coin and OK; "ONLINE" top left):
  - left, 游客: guest, nothing saved;
  - middle, 创造角色: new account.  Agree (OK), account number **6..11
    digits**, OK; password **6..11 digits**, OK; the same again, OK; then
    sex, skin, face and name;
  - right, 角色登录: account number (6..11 digits), OK, password (**4..11**),
    OK.  The QR code shown there logs in with
    `python -m igs_online wedance qr <account>`.

  Up cancels an entry, left deletes the last digit; too few digits shows
  输入数字不足 (not enough digits).  A password equal to the account number,
  or shorter than 6 digits, makes the game ask for a new one after login.  `python -m igs_online wedance add
  <account> <password>` makes an account on the server instead.
- **Saved:** everything the game sends: level, experience, counters,
  wardrobe and worn items, talks, props, pets, crystals, WeTickets, songs,
  missions; results feed the rankings.
- **After a member's login:** the play mode, 是否进入商城 (the market: keypad
  1 cards, 2 skins and faces, 3 talks, 4 tickets, 5 clothes, 6 lucky bags,
  0 leave), then the clothes screen, where keypad keys open the player
  pages (1 data, 2 schedule, 5 hidden songs, 6 achievements, 7 world tour,
  8 pets, 9 group).  Groups, missions and world settings are the server's
  (igs-online's README).
- **Versus:** on the opponent screen one cabinet opens a stage (OK on
  开新舞台, then 合作 co-op or 对战 fight); the other enters the screen after
  that and picks it.
- **Rankings in the attract:** Left and Right together switch the ranking
  pages to manual browsing.
- **Clock:** the server sends the real time, which the game ignores (it
  only sets clocks in 2008..2023); `settimeofday` and `stime` are no-ops and
  `hwclock` is not run (`IGS_NO_SYSTEM`).

## Extracting

`igs-mkromset wedanceonline2 <image>` builds the rom set (941 MB,
1.7 GB extracted).

| Path | What |
| --- | --- |
| p1 ext2 | LILO map, kernel `WeDo-OS-e64`; the launcher's GECA stub at sector 6693 |
| p2 | root filesystem (plaintext ROFS): `/bin/tcsh`, `/etc/rc.d/rc<d>.d` (GECA, pieces), `/usr/lib/libstdc++.so.5`, SDL_mixer and Vorbis in `/usr/X11R6/lib` |
| p3 | IGS_SHFS at `/WeDo/system`: `CFVER` (encrypted, below), fonts, `net/` (`net`, `pppd`, `pppoe`), `net.cfg`, `wedoCliPatch.exe` |
| p4 ext3 | `/WeDo/linux`: the data (`avi`, `dance`, `model`, `scene`, `sound`, `texture`, `script` (Lua), `ui`), `info/` (settings, bookkeeping), `patch/`, `wedo`; links `poweron`, `system`, `wedoCliPatch.exe` into `/WeDo/exec` and `/WeDo/system` |

The set keeps `wedo` as dumped and adds its unpacked copy at
`/WeDo/exec/wedo`, which the loaders run (cwd `/WeDo/linux`).

## How the cabinet starts it

1. The kernel mounts p3 on `/WeDo/system` and p4 on `/WeDo/linux`; `/bin/tcsh`
   and GECA build the launcher `/WeDo/exec/poweron` on a ramfs
   (`IGS_ROFS_REVERSE_ENGINEERING.md`).
2. `/etc/X11/IGS` starts the network daemon (`system/net/net`:
   `info/netsetting.dat` → `ifconfig`, `route`, `dhclient` or PPPoE),
   XFree86, the card reader's `pcscd` and `mwm`.
3. In `/WeDo/linux` it runs `./poweron` once, then `./wedo` in a loop.
   `poweron`:
   - applies a downloaded patch (`patch/<n>/<n>.lst`: add, replace, delete
     files) when `patch/ver.cfg` is newer than `info/ver.cfg`;
   - reads the 80-byte network registration from the IGS027A (packet
     18.1) into SysV shared memory, key 3293;
   - runs `system/wedoCliPatch.exe`, the patch client daemon (server in
     `info/wedoCliPatch.cfg`; it takes the registration from the shared
     memory).

The loaders run `wedo` alone: no launcher, daemon or patcher.

## The executable

`wedo` is UPX 3 (linux/elf386, NRV2E, no filters), 3,220,000 bytes packed,
8,254,965 unpacked, with symbols and debug information.  It links SDL 1.2,
SDL_mixer, GL, GLU, zlib, ncurses 5, librt and libstdc++ 5; built in are Lua,
SQLite 3.6.13, libcurl, libmad, a52dec, mpeg2dec, Xvid, qrencode and libiconv.
- **IGS027A** (`cmdport_*`, `pcicard_*`, `protect_*`, `igs036_*`): Go Go Ball
  2's packets and keys (`IGS_E2000.md`).
- **`/dev/tdcr`:** the kernel's file cipher; it decrypts `system/CFVER`
  (`version 212`, `versionname M212`).  The machine emulates it.
- **System status** (`main`): 0 start-up, 1 attract, 2 game, 3 test, 4 end;
  the IGS027A keeps it and answers each "next status".
- **Network** (`netapp_*`, `net_*`, `NetworkManager`): web files from
  `rf.igs.com.tw`, the store registration, the game server from `info/ip`
  (default `210.5.22.168`), pings.  Without them the game sets "no network"
  and runs offline: attract `demo_iNoNetProcess`, guest players
  (`card_iMakeNoCardPlayer`).
- **Shell commands:** `cp` of fonts, the loading picture, voices, effects and
  each song's music into `/tmp` (the cabinet's RAM disk), `rm` of them,
  `chmod`, `hdparm`, `aumix`.  With `IGS_NO_SYSTEM` the loaders run `cp` and
  `rm` into `/tmp` in-process and skip the rest; on Linux `/tmp` is an
  overlay in the set (`IGS_SET_DIRS=/tmp`).
- **Each song's start** (`main_iFreeRamSize`) reads a 2.4 kernel's
  `/proc/meminfo` and `./jlist /tmp`'s total, then fills the free RAM less
  50 MB.  The plugin provides both (96 MB free, an empty `/tmp`).
- **`/dev/ttyS1`:** the member-card reader; a null port here.
- CSV charts are read in text mode with CRLF lines; the game cuts the last
  character of the music name.

## Loader status

Done:
- [x] Rom set: `root p2; mount p3 /WeDo/system; assemble /WeDo/exec/poweron;
      mount p4 /WeDo/linux; unupx /WeDo/linux/wedo /WeDo/exec/wedo`.
- [x] `machines/e2000`: `/dev/tdcr`; files from the plugin
      (`igs_machine.vfile`).
- [x] `games/wedanceonline2`: keys, coins, system status, coin settings,
      password, network registration, versions, `/proc/meminfo`, `jlist`.
- [x] Online through igs-online: `IGS_NET` in both cores (Linux: `connect`,
      `sendto`, lookups; Windows: Winsock sockets, `win/net.c`); registration,
      login, members, results, rankings, QR login, account transfer, the
      market, lucky bags, missions, groups, the world tour, and versus /
      co-op between two instances seen working.
- [x] Key reporting: every press shown 4 frames, repeats separated.
- [x] Core: `IGS_NO_NET`; `cp` / `rm` into `/tmp` under `IGS_NO_SYSTEM`;
      `/tmp` overlay.  Windows: `getifaddrs`, `ftruncate64`, `utimes`,
      `getpwuid`, `__xpg_basename`; binary file modes (no CRT text mode).

Invented:
- [x] IGS027A version texts (`I:SHLE`, `O:SEMU`), serial 1, region 1,
      password 0000, fuses good; an empty network registration (store 0).

Open:
- [ ] Not seen: member cards, the camera, the patcher.
- [ ] Linux (Mesa): two of the game's GLSL shaders fail to compile
      (`operands of '==' must have the same type`); not seen on Windows.
- [ ] Lamps (packet 2) and meters (3) are ignored.

## Online (KoNet)

No UDP and no listening sockets; every connection is outgoing TCP from
`KoTCP_Conn`, so versus play also goes through the server.  Keys are in
`IGS_KEYS.md`.

| What | Where |
| --- | --- |
| Server-IP lookup | `rf.igs.com.tw:80` (fallback `202.133.245.140`), `GET /we5acserverip/wedo`: CRC16, game IP and port, second IP, patch IPs (CN, TW); a difference rewrites `info/ip` and `wedoCliPatch.cfg` (`net_iWebGetFile` `0x8180670`) |
| Game server | `info/ip` (`210.5.22.168:10000`, alternate `60.199.170.68`) (`netapp_iInit` `0x80D95E0`) |
| Banners | libcurl, `http://202.153.195.62:8080/wedo/{tw,cn}/*.dds` (`NetWeb::GetWebFile` `0x82E6400`) |
| Patch server | `wedoCliPatch.exe`, port 10001; the set is at patch 56 |
| Ping test | ICMP to `info/ping_data.lst`, results as 0x154 |
| Time | the login reply's time is set with `settimeofday` and `hwclock -w` (2008-07-07..2023-07-04 only) |

Framing (`KoTCP_SendMsg` `0x809A300`, `KoNetAP_MakeSend` `0x8077210`):
`u16 len` (1..18000; `0x4BFF` ping, `0x4FFF` reply, every 10 s), then
`u16 cmd & 0xFFF | mode << 12 | 0x4000` (more fragments; up to 6 MB).
Mode 0 plain, 1 AES, 2 zlib (`u32 rawlen, zlib, u16 crc`), 3 zlib then AES
(`u32 n` clear, then AES-ECB of `data, u16 crc`, zero-padded).  Fields are
little-endian, strings fixed-length.

Login (`net_iConnect` `0x817F820`, `net_vRecv` `0x817FBB0`):
0x11 (mode 3, static key; store id, versions, coin settings) → 0x12
(challenge, time, `g_wFeature`) → 0x13 (HMAC) → 0x14 / 0x163; then
AES-128 with keycpu.  Store id 0 first registers (`net_iReg`
`0x8181260`): 0x21 versions → 0x22 challenge → 0x23 HMAC, keycpu, keymac
→ 0x24 store id.

| Cmd | |
| --- | --- |
| 0x211 → 0x212 | player login (guest flag, or uid, account, password) → error byte or profile (`RecvPlayerLoginAck` `0x81C3F00`, exact length) |
| 0x21E | QR login: the server pushes a profile |
| 0x213, 0x215, 0x219, 0x21B, 0x21C | logout, create account, update info, password, transfer |
| 0x131..0x149 → 0x132..0x14A | rankings (replies zlib) |
| 0x165..0x16B → 0x166..0x16C | ticker, news, conditions, csvlist (0x16C replaces `csvlist.csv`) |
| 0x16F | writes a file |
| 0x151..0x154, 0x164, 0x171 → 0x172 | store bookkeeping |
| 0x161 → 0x162 | activity rate, mission check |
| 0x21C → 0x21D | account transfer (ARKOPAZE) |
| 0x222..0x226, 0x231..0x237, 0x241..0x243 | versus search, contests, song choice |
| 0x250 | versus relay (op byte: score, judgement, combo, pet, abort) |
| 0x251..0x253, 0x255 | fight ready, start, end; avatar |
| 0x261..0x26E → 0x262, 0x264, 0x26C | results, clothes, play log (`PLAYER_LOG`, 0x6F4 bytes, mode 3), missions; places |
| 0x271 → 0x272, 0x273 → 0x274, 0x275 | market trade, lucky bag, WeTicket |

The member-card reader (`reader_iInit`, ttyS1) is never called.  The QR
code names only the store and the time; the phone app completes it.
Every message layout found so far, with the profile, results, rankings
and versus, is in igs-online's `docs/WeDance.md`.  An empty message is
never delivered (`KoNetAP_Recv` returns 0): send at least one byte.
