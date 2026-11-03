# IGS keys and constants

Every key, mask and magic constant found so far, per game.  Each entry gives
its address so it can be re-checked.  The mechanisms are described in the
linked docs; this file is the lookup table.  The values the tools need
(ROFS key and profile, asset mask) are also in the machine-readable
software lists, `hash/<machine>.xml` (one `<software>`
entry per game id).

## Summary

| | Harpoon Lagoon | Ocean Party md105cn |
| --- | --- | --- |
| Machine | E2100 | E2100 |
| Kernel | `Fish4P-OS` (2.6.32.8) | `OceanParty-OS` (different build) |
| ROFS key literal | `Hunter4PFishV1.0` | `PartyOcean6PV1.0` |
| ROFS profile (extractor) | `fish4p` | `oceanparty` |

Scratch m101cn (`SwinV1.00Scratch`, profile `scratch`) and Hero of Robots
m100fa (`RobotsHeroOfV1.0`, profile `heroofrobots`) are listed under ROFS
below; their runtime protocols are in `Scratch.md` and `HeroOfRobots.md`,
the Hero channel and smart-card keys under Runtime channel.

| | Harpoon Lagoon | Ocean Party md105cn |
| --- | --- | --- |
| Asset cipher mask | `eea8543c6a16835d0900d7739f6bf77f` | `9528df6122f17c6ec8a249c24b63db9a` |
| ASIC firmware upload key | `Open pccard0 !!!` | `Start BigFish6P!` |
| Runtime channel | per-word XOR | RC4 per direction |
| Channel key, ASIC → game | `0x82675251` | `0x364251` |
| Channel key, game → ASIC | `0x80678267` | `0xA5148F` |

## ROFS (encrypted root filesystem, p2)

Details: `IGS_ROFS_REVERSE_ENGINEERING.md`.

**Common to both games:**
- **AES key.** AES-128 key = the 16-byte literal, split into four big-endian
  words `w0>>2, w1>>1, w2, w3<<1`.  The transform is in the kernel's key
  expansion, identical in both builds: Harpoon `0xC10201A4`, Ocean
  `0xC10208FC`.
- **AES itself** is standard: the S-boxes match FIPS-197.
- **Superblock magic** after decryption: `0xD4AA2684`.
- **Field XORs.** The kernel's `igs_rofs` init (`0xC1C64926` in both)
  stores five 32-bit constants.  Each field XOR is a constant ± a per-kernel
  delta.

### Harpoon Lagoon (`Fish4P-OS`)

- **Key literal:** `Hunter4PFishV1.0` at VA `0xC18CF847`.  Effective AES
  key: `12 1D 5B 9D 32 B9 1A 28 46 69 73 68 AC 62 5C 60`.
- **Init constants:**

  | Address | Value |
  | --- | --- |
  | `C1CACAF4` | `0x3CED9653` |
  | `C1CACAF8` | `0x9F3F2429` |
  | `C1CACAFC` | `0x3A9A818C` |
  | `C1CACB00` | `0x90CF6AC4` |
  | `C1CACB04` | `0x849C446F` |

- **Delta:** `0x3CE8` (subtracted).
- **Field XORs:**

  | Field | XOR |
  | --- | --- |
  | name length | `0x3A9A44A4` |
  | size | `0x3CED596B` |
  | table | `0x9F3EE741` |

- **Magic:** decrypted block at p2+`0xC8`, word 1.

### Ocean Party (`OceanParty-OS`)

- **Key literal:** `PartyOcean6PV1.0` at VA `0xC18CFBE9`.
  - It is copied twice: to `0xC192E4C0` for the metadata (AES-ECB) and to
    `0xC192E4D0` for the names (AES-CBC).
- **Init constants:**

  | Address | Value |
  | --- | --- |
  | `C1CACCE8` | `0x03CC0182` |
  | `C1CACCEC` | `0x1967C92D` |
  | `C1CACCF0` | `0xBCC26C88` |
  | `C1CACCF4` | `0x169787C0` |
  | `C1CACCF8` | `0x02C4A96B` |

- **Delta:** `0x805E`.
- **Field XORs:**

  | Field | Derivation | XOR |
  | --- | --- | --- |
  | name length | `CCF8 − 0x805E` | `0x02C4290D` |
  | size | `CCF8 + 0x805E` | `0x02C529C9` |
  | table | `CCE8 − 0x805E` | `0x03CB8124` |

  A flag at `byte_C192E4E0` switches the table delta to `+`; it is not set
  in this image.
- **Record checks:**
  - `dec(R+0x10)[1] ^ CCEC == 0xA5A5A5A5`
  - `dec(R+0x20)[3] ^ CCF0 == 0xAA55EB48`
- **Superblock checks** (mount `0xC101F792`):
  - `(CCE8 ^ x) == 0x1AABC8AF`
  - `(x ^ 0x8426AAD4) == 0x87EAAB56`
  - `(CCF4 ^ x) == 0x14532EAB`
  - `(CCF0 ^ x) == 0xAA55EB48`
- **Magic:** decrypted block at p2+`0x58`, word 0.
- **Also present:** a modified RC4 at `0xC10202FC` and `0xC102041C`.
  - Key schedule: `j += (S[i]+3) + key[k] + 17`, and the swapped value
    keeps its `+3`.
  - It is not used by the filesystem path; its purpose is unknown.

### Scratch m101cn (`Scratch-OS`)

- **Key literal:** `SwinV1.00Scratch` (kernel payload file offset
  `0x8D3649`); copied raw to `0xC19324C0` (ECB) and `0xC19324D0` (CBC).
  Effective AES key: `14 DD DA 5B 2B 18 97 18 30 53 63 72 C2 E8 C6 D0`.
- **Init constants** (`init_igs_rofs_fs` `0xC1C72926`): C0 `0xE1ED4C82`,
  C1 `0x3727876F`, C2 `0x928222CA`, C3 `0x38D7C982`, C4 `0x2C84E729`.
- **Delta:** `0xA631`.
- **Field XORs:** name length `dec(R)[3] ^ 0x2C858D5A` (C4 + delta), size
  `dec(R+0x30)[1] ^ 0x2C858D5A`, table `dec(R+0x50)[1] ^ 0xE1ECA651`
  (C0 − delta).
- **Record checks:** `dec(R+0x20)[1] ^ C1 == 0xA5A5A5A5`,
  `dec(R+0x60)[2] ^ C2 == 0xAA55EB48`.
- **Superblock:** magic in the unaligned block at `0x74`, word 2.  Checks:
  `dec(0x84)[3] ^ C0 == 0xD6CACBED`, `dec(0x64)[2] ^ 0x8426AAD4 ==
  0x65CBE656`, `dec(0xB4)[1] ^ C3 == 0x14532EAB`, `dec(0xF4)[2] ^ C2 ==
  0xAA55EB48`.
- **Per-file CRC-32C** (Castagnoli, init 0, no final XOR) over the stored
  extents, from `dec(R+0x10)`; skipped when words 2/3 are `0xA5A5A5A5` /
  `0xAA55EB48`.
- **Executable layer:** `/Scratch/swin` is FPGA-cipher encrypted.  At boot
  the kernel (`0xC100133A`) runs `/bin/unrar /Scratch/swin /Scratch.bin/swin
  Hello-ScratchWin`: the key string goes to the FPGA (`0x40106606` between
  `0x6607`/`0x6608`), the derived key comes back with `0x80106605`, then
  `0xC0106610` decrypts 64 KiB chunks.  The FPGA cipher's mask for that key
  is `e4be1b4e7f081654fa6b81698b039a06`, recovered from the known ELF
  header.  The decrypted file is 1,801,508 bytes (12 bytes of padding
  removed).

### Hero of Robots m100fa (`HeroOfRobots-OS`)

- **Key literal:** `RobotsHeroOfV1.0` at VA `0xC1912752`.  Effective AES
  key: `14 9B D8 9B 3A 39 A4 32 72 6F 4F 66 AC 62 5C 60`.
- **Init constants** (`0xC1CB8D59`, stored at `0xC1D09BB4..C4`): C0
  `0xFFF217D3`, C1 `0xF487BDBA`, C2 `0x5122181F`, C3 `0xFB77F357`, C4
  `0xEF24DDFC`.
- **Delta:** `0x265B`.
- **Field XORs:** name length `dec(R+0x40)[2] ^ 0x51223E7A` (C2 + delta),
  size `dec(R+0x30)[0] ^ 0xEF250457` (C4 + delta), table
  `dec(R)[2] ^ 0xF487975F` (C1 − delta; the root record alone uses
  `0xF487E415`, C1 + delta).
- **Record checks:** `dec(R+0x10)[1] ^ C1 == 0xA5A5A5A5`,
  `dec(R+0x20)[1] ^ C2 == 0xAA55EB48`.
- **Superblock:** magic at block `0xF0`, word 3.  Checks:
  `dec(0xE0)[1] ^ C0 == 0x0B75AA69`, `dec(0xC0)[0] ^ 0x8426AAD4 ==
  0x7BD4BD07`, `dec(0x50)[1] ^ C3 == 0x14532EAB`, `dec(0x60)[1] ^ C2 ==
  0xAA55EB48`.
- Names are plaintext with junk padding; the page table and data are plain.

## E2000: Gold Captain (`GoldCaptain-OS-e65`, Linux 2.4.31)

Details: the E2000 part of `IGS_ROFS_REVERSE_ENGINEERING.md`.  Nothing is
encrypted on disk; the constants below are integrity checks and the
executable-assembly recipe.

| Item | Value | Where |
| --- | --- | --- |
| Root ROFS magic | `0xD4AA2681` at p2+`0x4C` | kernel `0xC04849F1` |
| Root ROFS minor check | `sb[0] ^ magic == 0x55AA48EB` | `0xC0484A28` |
| Game type check | `sb[4] ^ root_version == 0xAD2423DB` (root_version `0xDB5A3430`) | `0xC0484A69` |
| Inode version check | `inode[0] ^ root_version == 0xEC3BC248` (inodes store `0x3761F678`) | `0xC04845ED` |
| IGS_SHFS magic | `0xD4AA2682` (`…83` byte-swapped) | `0xC0488506` |
| IGS_SHFS game type / version | `0xAD2423DB` / `0xEC3BC248` | `0xC04884B9` / `0xC04884D9` |
| IGS_SHFS major / minor | `0xDE5550B3` / `0x8448B33B` | `0xC0488C59` / `0xC0488C79` |
| Stage-2 stub (GECA) | p1 sector 6813, 1 KiB, + `/lib/libdb0.a` | `/bin/csh` `sub_80481D0` |
| Executable head | p2 sector 125515, 1 KiB | same |
| Piece order / trim | `47632518`, 1004 KiB, −17 per piece | GECA `0x804B54C` |
| Product string | `GoldCaptian-E2000-v1-e65` (sic) | csh, GECA |
| IGS036 message-key RC4 keys | `PRIGS036\0` (IGS036 → game), `PR_LINUX\0` (game → IGS036); standard RC4, continuous stream per direction | game `sub_80CCA10`, RC4 `sub_8070940` / `sub_8070610` |
| Message key | `rand() % 100000`, XORed over the payload words | `sub_80CD050` / `sub_80CD100` |
| BIOS fingerprint port | `/dev/roio` ioctl `0xFC`, address `^ 0x1FB8408E`, data `^ 0xB8` | game `sub_813B760` |
| Accepted BIOS (group 1) | `0xFE0C1` `L4S5MG3`, `0xFEC84` `6A6IXE19C-00`, `0xFECDF` `I6A6IXE19` | table `0x8173320` |
| PLX card | PCI9030, vendor `0x10B5`, serial `Pci9030-0`, SDK 4.3.0 | `sub_80CE840`, `sub_806B9D0` |

## E2000: Pirate Revenge m101cn (`PirateRevenge-OS-57P`, Linux 2.4.31)

Same filesystems and integrity constants as Gold Captain (the IGS_SHFS
superblock is the plain layout, without game type or version).  Game
addresses are in p3's `sship` (symbols kept).

| Item | Value | Where |
| --- | --- | --- |
| Stage-1 loader | `/bin/HWtest`, argv `/etc/rc.d/rc0.d __KERNEL__ "-no-print -PR"` | kernel strings `0x3DC20D` (decompressed) |
| Executable head | p1 sector 6813, 1 KiB | HWtest `sub_80481D0` |
| Stage-2 stub (GECA) | p2 sector 125515, 1 KiB, + `/etc/rc.d/rc0.d` | same |
| Pieces / skip | `/etc/rc.d/rc<d>.d`, order `6415237`, skip 9 KiB, +4 per piece | GECA `0x804B34C` (skip, step, mode 1 = add), `0x804B358` (order) |
| Product string | `PirateRevenge-E2000-v05-e61` | HWtest, GECA (`--james`) |
| IGS036 message-key RC4 keys | `PRIGS036\0`, `PR_LINUX\0` (as Gold Captain) | `ShareramOperator_ResetRC4Keys` `0x80CCE80` |
| Message key | `rand() % 100000` | `ShareramOperator_BufferToShareram` `0x80CD360` |
| BIOS fingerprint | `/dev/roio` ioctl `0xFC`, `^ 0x1FB8408E` / `^ 0xB8`; group 1 as Gold Captain | `BIOS_CRC` `0x813A4D0`, table `motherboard_serial_number` `0x8171760` |
| Settings | 754 words (`SaveData` `0x8796480`); disk copy `/fec_data/gameset.bin` = `SHIP_800` + `0xBC8` bytes | `SettingSaveData` `0x8076F20` |
| Rudder calibration | settings words 742, 743, 744 (`GameRudder_Init(w743, w742, w744)`) | `SettingLoadData` `0x80771B0` |
| Key bits | 0 right, 1 left, 2 bomb, 3/4 coin, 5 test, 6 service, 7 ticket | `CRKey_Init` `0x80CBA30` |

## E2000: Go Go Ball 2 DX (`GoGoBall2-OS-e63`, Linux 2.4.31)

Same filesystems as Gold Captain; IGS_SHFS game type `0x5CAAEC43`, version
`0x55BBF972`, major `0x2FDB9F2B`, minor `0x3DC88801`.  Game addresses are
in the assembled `pitch` (stripped).

| Item | Value | Where |
| --- | --- | --- |
| Stage-1 loader | `/bin/csh`, argv `/etc/rc.d/rc0.d __KERNEL__ "-no-print -GGB2"` | kernel strings |
| Stage-2 stub (GECA) | p1 sector 6692, 1 KiB, + `/etc/rc.d/rc0.d` | csh |
| Executable head | p2 sector 129912, 1 KiB | csh |
| Pieces / skip | `/etc/rc.d/rc<d>.d`, order `2163547`, skip 168 KiB, −17 per piece; ramfs `/GoGoBall2/linux`, file `pitch` | GECA `0x804B3CC` (skip, step, mode 2 = subtract), main `0x8048718` |
| Product string | `GoGoBall2-E2000-v1-e63` | csh, GECA (`--james`) |
| IGS027A packet RC4 keys | 12 bytes `EncodeGGB   ` (game → chip, state `0x8546260`), `DecodeGGB   ` (chip → game, `0x8546680`); standard RC4, continuous | `sub_80832F0`; RC4 `sub_808BB50` / `sub_808B800` |
| Packet checksum | word 17 = sum of words 18..57 (plaintext) | `sub_8083470`, `sub_80834E0` |
| Re-key (type 16) | 20 bytes: function 0 mday, hour, min, sec, ms (chip → game); function 1 ms, sec, min, hour, mday (game → chip) | `sub_8083550`, `sub_8083610` |
| Mailbox | BAR3 byte 0 = sequence (1..15) \| port (`0x20`, `0x30`, `0x50`); chip answers `next \| 0x40`, start-up `0x80`; doorbell BAR3+14 | `sub_806B3D0`, `sub_806B290` |
| BIOS fingerprint | as Gold Captain (group 1) | table `0x81E3560`, check `sub_81A1B10` |
| Light-curtain calibration | factory `390, 292, 187, 249, 0, 800, 0, 600, 0, 78, 0, 46` | `0x81E5EE0` |
| Key bits (type 13) | 1 KEY2, 2 KEY1, 6 test, 7 service, 30 coin | `sub_8063A10` |
| Region (type 18, u16 at 8) | 1 `0CN`, 2 `0HK`, 3 `0FA`, 4 `0US`, 5 `0KR`, 6 `0TL`, other `0TW` | `sub_8072270` |

## E2000: Speed Rider m109cnc (`SR-E2000-v1-e64`, Linux 2.4.31)

IGS_SHFS game type `0x3AEF3EEC`, version `0xF14C5AEA`, major `0x499E4D84`,
minor `0x993F2B99`.  Game addresses are in the assembled `SR` (stripped).

| Item | Value | Where |
| --- | --- | --- |
| Stage-1 loader | `/bin/csh`, argv `/etc/rc.d/rc0.d __KERNEL__ "-no-print -SR"` | GECA `main` |
| Stage-2 stub (GECA) | p1 sector 7145, 1 KiB, + `/etc/rc.d/rc0.d` | csh |
| Executable head | p2 sector 114921, 1 KiB | csh |
| Pieces / cut | `/etc/rc.d/rc<d>.d`, order `147568329`; skip `s` = 17 KiB +3, drop `t` = 413 KiB −9 per piece; ramfs `/sr/exec`, file `SR` | GECA `0x804B5AC` (s, step, mode 1 = add, t, step, mode 2 = subtract), `sub_8048958` |
| Product string | `SR-E2000-v1-e64` | GECA (`--james`) |
| IGS036 message RC4 | 8 bytes `ILoveIGS` (no NUL), one continuous stream for both directions; sub-message 1 re-keys with entry n of the 9-byte key table | init `sub_82D9B20`; table `0x893D5C0`; RC4 `sub_82DB940` / `sub_82DBA40` |
| Message key | header +8 (RC4); words +32..+60 and the sub-messages XOR it | `sub_82DA7A0`, `sub_82DAE40` |
| Sequence | status = BAR3 register 3 low byte; the game writes `(s+1) \| 0x80` to registers 0..6, then `s = (that+1) \| 0x80` | `sub_82DA590`, `sub_82DA620`, `sub_82DA650` |
| Sub-message | `[len, type, data…, (sum of the others) \| 0x80]`, a 0 length ends the list | `sub_82DA670`, `sub_82D99F0` |
| Versions (sub-message 3) | words 1, 2, 3, 0 = `0, 1, 0x30001, 100`; 5, 6, 7, 4 = `0, 1, 0x30001, 109` | `sub_832C120` |
| NVRAM blocks | 264, 660, 3604, 1444, 1444 bytes | `0x893D944` |
| Controls calibrated | block 0 byte 135 (version word 3 at byte 0) | `sub_830E9B0`, `sub_83031C0` |
| Zip prefixes | 2000 bytes per archive; index by name: 0 `OGRE.`, 1 `MENUUI.`, 2 `RACEUI.`, 3..6 `GREATDAM.` `SHANGHAI.` `TAIPEI.` `HEYLONG.`, 7..10 the `A` variants, 11 `FACTORY.`, 12 `CHAMPION.` | `sub_82D9D70`, `sub_82DA320` |
| Button events (0x30/0x31) | chip button → index: 6, 2, 3, 0, 1, 7, 5, 4 | `0x893D480` |
| Member-card bit order | 672 words (sub-message 0x42); code alphabet 128 bytes | `sub_846AA30`, `0x8948AE0` |
| Model numbers | `M:M-109CNC`, `O:S-109CNC`, `I:S-100CNC` | static init `sub_80BBB40` |

## E2000: Speed Driver 2 (`SD2-E2000-v05-e61`, Linux 2.4.31)

IGS_SHFS plain layout (major 2, minor 1).  Game addresses are in the
assembled `sd2` (stripped).

| Item | Value | Where |
| --- | --- | --- |
| Stage-1 loader | `/bin/HWtest`, tag `-SD2` | |
| Stage-2 stub (GECA) | p2 sector 114921, 1 KiB, + `/etc/rc.d/rc4.d` | GECA constants `0x804B54C` |
| Executable head | p1 sector 7145, 1 KiB | HWtest |
| Pieces / cut | `/lib/libc<d>.so`, order `143287569`; skip `s` = 3 KiB +4, drop `t` = 141 KiB −9 per piece; ramfs `/race/exec`, file `sd2` | GECA `0x804B54C` |
| IGS036 message RC4 | 8 bytes `ILoveIGS`, chip → game only, never re-keyed | init `sub_810FE68`; RC4 `sub_808D438` / `sub_808DD42` |
| Message key | header +20 (RC4); the sub-messages, the game's length (+24) and state (+60) XOR it | read `sub_81102C2`, write `sub_8112496` |
| Sequence | as Speed Rider; status also at BAR2 +0x3000 | `sub_811013C`, `sub_81101D2`, `sub_8110208` |
| Sub-message | `[len, type, data…, (sum of the others) \| 0x80]`, a 0 length ends the list | `sub_810FB26`, `sub_811021A` |
| `data.hdr` prefix (0xB2) | 128 bytes: u32 143 (directories), records `{u32 first file, name}` `data`, `data\2d` (0), `data\2d\ui` (0), `\mark` (39), `data\2d\operator` (68), `data\2d\effect` (72), `data\2d\effect\lensflare` (72, no NUL); the file's first byte is the XOR of the prefix (`0xDD`) | `IGS_PK::LoadHead` `sub_808CBF0` |
| `data.ZPK` | `zlPack 0002`; entries: u32 size, zlib | |
| Store settings (0x39) | free play, start coins 1P/2P, continue coins 1P/2P | `sub_8114B88` |
## E2000: Panda Family (CN) (`PF-E2000-v1-e65`, Linux 2.4.31)

IGS_SHFS game type `0x2AD566DC`, version `0xEDEB1E16`, major `0x59A415B4`,
minor `0x85986F65`.  Game addresses are in the assembled `PF` (stripped).

| Item | Value | Where |
| --- | --- | --- |
| Stage-1 loader | `/bin/csh`, tag `-PF` | |
| Stage-2 stub (GECA) | first KiB of `/bin/tcsh` + `/bin/ksh` (feature `geca_head`) | GECA constants `0x804B5CC` |
| Executable head | p1 sector 6658, 1 KiB | csh |
| Pieces / skip | `/lib/libfun<d>.so`, order `413256`, skip 2 KiB, +12 per piece; ramfs `/linux`, file `PF` | GECA `0x804B5CC` |
| IGS036 packet RC4 | 7 bytes `RC4_36` (chip → game, state `0x86E17A0`), `RC4_PC` (game → chip, `0x86E1C00`), NUL included; OpenSSL RC4 over the checksum word only, continuous per direction | `sub_80726A0`; RC4 `sub_804DEA0`, `RC4_set_key` `sub_804DDA0` |
| Packet checksum | 32-bit sum of bytes 4..n−1; those bytes XOR its low byte | `sub_8072880`, `sub_8072990` |
| Status | BAR3 registers 0..3 low bytes: sequence \| length << 8; game writes sequence + 1 to registers 0..6, doorbell BAR3+14; first sequence 1 | `sub_804E4D0`, `sub_804E470`, `sub_804E360`, `byte_81C19B4` |
| Sub-message handler | `[type, length u16, data]` | `sub_8072E40` (`ShareRAM_36Packet_Handler`), add `sub_8072820` |
| Key bits (type 2) | 0..3 1P, 4..7 2P, 8 service, 9 test; keyboard fallback `asdfhjklv678` | `sub_8072B90`, table `0x8153F00` |
| Settings | 3132 bytes at `0x86EF2A0`; check `sub_8077C90`; defaults `sub_8078C20`: 2 coins / 1 credit, password 1234, volume 4/4, tickets on, 5..15, camera on | load `sub_8078EF0`, store `sub_8078FC0` |
| Models | `M:M%03d%s` (`M100CN`), `I:S%01d%02d%s` / `O:S…` from types 5 / 6 | `sub_8077D60` |
| Camera | `/proc/video/dev/video0..4`: `TP6800`, `TP6810`, `OV511`, `IP293 USB Camera` → types 1..4; photo when found and the setting is on | `libIGS_API` `CameraInfo_Read`; `sub_8068470`, `sub_808C2D0` |

## E2000: We Dance Online 2 (`WeDo-e2000-v1-e64`, Linux 2.4.31)

IGS_SHFS game type `0x8E1673C2`, version `0xB30596F1`, major `0xFD6700AA`,
minor `0xDB76E782`.  Game addresses are in the unpacked `wedo` (symbols
kept); launcher addresses in `poweron` (stripped).

| Item | Value | Where |
| --- | --- | --- |
| Stage-1 loader | `/bin/tcsh`, tag `-WeDo` | |
| Stage-2 stub (GECA) | p1 sector 6693, 1 KiB, + `/etc/rc.d/rc0.d` | GECA constants `0x804B5EC` |
| Executable head | p2 sector 129918, 1 KiB | tcsh |
| Pieces / skip | `/etc/rc.d/rc<d>.d`, order `314256`, skip 299 KiB, −3 per piece; ramfs `/WeDo/exec`, file `poweron` (the launcher) | GECA |
| `wedo` packer | UPX 3: l_info version 13, format 12 (linux/elf386), NRV2E (method 8), no filters; 3,220,000 → 8,254,965 bytes; original entry `0x8055670` | `romtool/igs_upx.py` |
| IGS027A packet RC4 keys | 12 bytes `EncodeGGB   ` (game → chip), `DecodeGGB   ` (chip → game), as Go Go Ball 2 | `protect_vKeyInit` `0x807C580` |
| Re-key (type 16) | as Go Go Ball 2 | `protect_vEncodeKeySet` `0x807C7B0`, `protect_vDecodeKeySet` `0x807C870` |
| `/dev/tdcr` cipher | RC4 variant: KSA `t = S[i] + 3; j += key[k] + t + 0x11; S[i] = S[j]; S[j] = t` (32-bit S), standard PRGA on the words' low bytes; key `abcdef123456` (12 bytes) for `system/CFVER` | kernel `cpu/tdcr` fops `0xC05C8B20`: ioctl `0xC01B9B88`, KSA `0xC01B9CB0`, PRGA `0xC01B9DB0`; game `protect_pvMalloc` `0x807CFA0` |
| `/dev/tdca`, `tdcm`, `tdc3` | modes 2..5 (key `1234` for 2, 3); unused by the game | `protect_prfopen`, `protect_pvMalloc` |
| BIOS fingerprint | `/dev/roio` as Gold Captain; group 5: `0xF086E` and `0xFEC7C` `i852-W83627HF`, `0xFEC8A` `6A69YILTC-00`, `0xFECDE` `Ph6A69YILT` | `bios_serial_number` `0x852B9A0`, `BIOS_CRC`; launcher table `0x80507C0` |
| Key bits (type 1) | 0 OK, 1 up, 2 down, 3 right, 4 left, 5 service, 6 test; keypad 15 `1`, 11 `2`, 7 `3`, 16 `4`, 12 `5`, 8 `6`, 17 `7`, 13 `8`, 9 `9`, 18 `*`, 14 `0`, 10 `#` | `m_saKeyMap` `0x8658060`, `key_iTest` `0x805D100` |
| Region (type 14, u16 at 8) | 1 China, 2 Hong Kong, 3 other, 4 USA, 5 Korea, 6 Thailand, other Taiwan; language 1 CN for 1, 2 EN for 4..6, else TW | `version_iInit` `0x8081420` |
| Network registration (type 18) | 80 bytes: store id, CPU key, MAC key, net and program versions (`"7"` at 28); copied to SysV shm key 3293 by the launcher | `net_iInit` `0x817F3E0`; `poweron` `main` `0x804A430` |
| Book-keeping file | `info/bookEnc.bin`: byte `i` XOR `(i − 20)` over 1040 bytes | `book_iIgs036Read` `0x809D760` |
| KoNet static AES-256 (ECB) | `KONETv0.1IGS2008` + `B0 DF BB 52 BF 57 B4 4C` + `ARCADE10`: `4B4F4E455476302E3149475332303038B0DFBB52BF57B44C4152434144453130`; login 0x11 and registration 0x21–0x23 | `net_iConnect` `0x817F820`, `net_iReg` `0x8181260` |
| KoNet session AES-128 (ECB) | the cabinet's keycpu (registration +4, 16 bytes; default `4a0f0000000802069d640000fffbebbf`) | `net_vRecv` `0x817FBB0` |
| KoNet HMAC-MD5 | key keymac (registration +20, 6 bytes; default `00145e7ea824`) ‖ keycpu, over the 8-byte challenge reordered `c0 c2 c4 c6 c1 c3 c5 c7` | `net_vRecv`, `net_iReg` |
| KoNet keycpu, keymac at registration | CPUID leaf 1 (`eax ebx ecx edx`) with bytes 0, 3, 4, 8, 12, 15 set to MAC bytes 5, 4, 3, 2, 1, 0; keymac the `eth0` MAC (`SIOCGIFHWADDR`); before the IGS027A read: store 10000000 with the defaults above | `net_iInit` `0x817F3E0` |
| KoNet blob check | CRC-16/ARC of the data XOR `lo16(n)` XOR `hi16(n)` | `_netAP_enAES`, `_netAP_enZ` |
| Keypad numbers (accounts, passwords) | `u64` = decimal value \| digit count << 56; Up = `0x4000000000000000` (cancel) | `PDTLogin::InputAccount` `0x81CFA80` |
| QR login text | `"%llu-%d"`: `(32 · (store − 10000000)) ^ 0x85B8060F9D12F`, `time ^ 0x1EDBAA25` | `QRcodeMng::Init` `0x82C5060` |
| Downloaded files | 32 hex MD5 + 1 byte, then the data | `NetWeb::GetWebFile` `0x82E6400` |

## E1000: Percussion Kids M105CN (Linux 2.4.24-gx, uClibc 0.9.26)

Nothing is encrypted on disk: five ext2 partitions (p4 the extended one).
Game addresses are in `pkids` (symbols kept).  Details: `IGS_E1000.md`.

| Item | Value | Where |
| --- | --- | --- |
| a27 packet encode table | 2 rows × 256 bytes; data byte `i` (of the first 256) XOR row `key >> 7`, index `((key & 0xEF) + i) & 0xFF`; row 0 starts `31 CB E0 FE 35 3D 59 EB`, row 1 `9C ED 8E E5 17 3C DC 68` | `m_baaEncodeTable` `0x807E760`, `cmdport_vDecode` `0x805DF3C` |
| a27 packet checksums | +0x26: low byte of size + mode + bytes +8, +9, +0xE, +0x20, words +0xA, +0xC, +0x22, key; +0x27: 2 × that + data bytes 0..11 after decoding | `cmdport_vCheckSumCheck` `0x805DF90` |
| Answer checksums | +0xC / +0xD: the same over the answer header, its date and tick | `cmdport_vCheckSumSet` `0x805E048` |
| a27 start | 0-byte write, mode 29, at `lseek(254)` | `cmdport_vCmdPortInit` `0x805DCEC` |
| Status byte (read's return) | `0xF0`, `0xFD`, `0xFE` PC error, `0xF2` debug, `0xF3` value error, `0xF4` interrupt error, `0xF5` time-out, `0xFF` assert | `cmdport_vCmdPortValueCheck` `0x805DD60` |
| Region (command 3) | 1 China, 2 Hong Kong, 3 other, 4 USA, 5 Korea, 6 Thailand, 7 other without card, else Taiwan; version suffix `05CN`, `05HK`, `05FA`, `05US`, `05KR`, `05TL`, `05NC`, `05TW` | `version_vVersionInit` `0x8061290` |
| GX2 PCI IDs | `0x0028100B` (bus 0 dev 1 fn 0), `0x0030100B` (fn 1); BARs at config `0x80000910` (frame buffer), `…914` (GP), `…918` (DC), `…91C` (VP) | `gfx_detect_cpu` `0x807475C` |
| VSA registers | `0xAC1C` ← `0xFC53`, index; `0xAC1E` → data: `0x0200` frame buffer / 512 KB, `0x1201` core MHz | `gfx_get_frame_buffer_size` `0x80745F4`, `gfx_get_core_freq` `0x8074580` |
| GP alpha modes | `0x740000 \| a` blend, `0x460000` source (a = 255), `0x470000` destination (a = 0); colour key flag `0x800` | `gfx2_set_alpha_value` `0x8075C04`, `gfx2_set_source_transparency` `0x8075BBC` |
| Song table | `g_raBand1List` `0x8083A80`, 31 × 100 bytes: song struct, base rank, MTV rom path, wav list and count | `song_prRomSongGet` `0x8068984` |
| Song struct | +0 tempo × 100, +2 length (ticks), +5 scroll speed (px per tick), +6 name, +8 rank; +100 36 track pointers | `song_wSongTempoGet` `0x80671CC` |
| Chart event | u32: bits 0..13 tick (1/8 beat; `0x3FFF` ends a track), 14..23 wav index, 24..31 0 note / 1 roll start / 2 roll end | `song_vSongDataInit` `0x8067344` |
| Record files | `record/<name>_<rank>_<n>.rec`: 0x66-byte header, then `WAVEDIR `, `TREC1   `, `TREC2   `, `TRECBG  `, ` FILEEND` chunks (none on the disk) | `record_vRecordReadFromFile` `0x80714E0` |

## E2200: Percussion Master 3 M107FA (`PM3-OS`, Linux 2.6.32.8)

Details: `IGS_E2200.md`, the E2200 part of
`IGS_ROFS_REVERSE_ENGINEERING.md`.  Kernel addresses are in the
decompressed `PM3-OS`, launcher addresses in the decrypted `PowerOn`, game
addresses in the decrypted `pm3` (dynamic symbols).

| Item | Value | Where |
| --- | --- | --- |
| ROFS key literal | `E2200-PMaster-V3` (VA `0xC1931EB4`); effective AES key `11 4C 8C 8C 18 16 A8 26 61 73 74 65 E4 5A AC 66` | `init_igs_rofs_fs` `0xC1CF0D59` |
| ROFS init constants | C0 `0xCD84A430`, C1 `0xEC294A08`, C2 `0x498CEFAD`, C3 `0xE3D904E5`, C4 `0xF78A2A4E` (stored at `0xC1D42DA8..DB8`) | same |
| ROFS delta | `0xED2E` | `sub_C101FD38` |
| ROFS field XORs | name length `dec(R+0x20)[1] ^ 0x498C027F` (C2 − delta), size `dec(R+0x50)[3] ^ 0xCD83B702` (C0 − delta), table `dec(R+0x40)[0] ^ 0xEC285CDA` (C1 − delta; C4 + delta `0xF78B177C` behind a flag) | readdir `sub_C10205E5`, `sub_C101FD38`, inode `sub_C101FE06` |
| ROFS record checks | `dec(R)[3] ^ C1 == 0xA5A5A5A5`, `dec(R+0x60)[1] ^ C2 == 0xAA55EB48` | `sub_C101FE06` |
| ROFS superblock | magic `dec(0xD8)[1]`; `dec(0xB8)[2] ^ C0 == 0x21ADEE38`, `dec(0xE8)[1] ^ 0x8426AAD4 == 0x49A20EE4`, `dec(0x40)[3] ^ C3 == 0x14532EAB`, `dec(0xA0)[1] ^ C2 == 0xAA55EB48`; root mode at `+0xB4`, features at `+0xFC` | mount `sub_C1020225` |
| ROFS layout | Ocean Party's: header `0x78`, mode `+0x74` (uid `+0x70`, gid `+0x72`), names AES-CBC (zero IV), page table one encrypted block per page (end offset word 1), zlib headers blanked; root offset `0x150` or `0x350` | readpage `sub_C1020A56`, inflate `sub_C1020BF0` |
| Launcher decrypt | `/bin/uudecode /PM3/PowerOn /PM3.bin/PowerOn Hello-Percussion`; FPGA mask `2b36333bfe90358941f1c3dbc5399ea3` (from the ELF header) | kernel `sub_C100133A`, strings `0xC1922932` |
| Executable decrypt | `PowerOn` key table (16 bytes × 10, repeated for ids 10..15) at `0x804FC20`; key id bits: byte 0 bit 3, byte 1 bit 2, byte 2 bit 1, byte 3 bit 6; `pm3` key id 5, mask `3a0e2d44e578a0a81370dd147cbf2e19`; trailer {random, size, id} | `sub_804B61D`, `main` `0x804C22A` |
| `PowerOn` keys 0..9 | `4e67d598e73e6a6f671d87cda713b27c`, `332d473d27110272eef7a518e51f4a62`, `66118fad6703128c5a2dffb25c721e5e`, `3e07554807ae5a1f3a9d3e2278912e0e`, `3a8d7e5277a2ce3e9e687598e6eeca2f`, `45578593075e675fc47df93279029e36`, `e74d67ced71bb36c43628538ef3b2a66`, `618f87546544753d575a87fd37134274`, `62ed67fdaf6322bcc2edf63ecc6243ae`, `61603e59819475d3c00d7e92769947ce` | `0x804FC20` |
| Chart masks, keys 0..9 | `3258217a79e88261f139e5adc73d611a`, `d882845fd9c684ab158ff1281df44a03`, `890fe518073ea7283532720c5696da58`, `161de4f0b210bc08d6227528d6ffa952`, `958066ea688ce5d9eece582a5fe95a51`, `3a0e2d44e578a0a81370dd147cbf2e19`, `1803a73fa9fee3e0282df9209d249392`, `9adab82c2da8a0ea30bf41a3663c637a`, `98cfef1ac9b689a452ece22a53945a55`, `cf97d6c82769984790a0e170729b1b31` (59, 62, 96, 91, 79, 82, 98, 104, 78, 65 charts) | charts' statistics and trailers |
| Chart key id bits | word 0: bit 4, bit 10, bit 17, bit 29 (weights 1, 2, 4, 8) | `AES::iAESDecrypt` `0x86252C0` |
| Chart cut plaintext | `#------ BASE DAT` (every chart's first 16 bytes) | `BandScript::iLoadBandFile` `0x85ED050` |
| `SongList.enc` key | `3e07d297578e4a7f471d83c86793326c` (first 16 of 32 bytes); mask `1618a1da69c82069a53bc92cc6782552` | `0x98A2A80`, `BandScript::iLoadList` `0x85ECB60` |
| IGS036 upload key | `PM3FA Start Now!`; image `A36ROM` (`0x98A2AC0`, `0x80000` bytes) | `Igs036P2A::Igs036P2A` `0x8628FA0` |
| IGS036 channel RC4 | key schedule on 12 bytes `PM3   IGSA36`, then on the game's 8 random bytes (message 0); standard RC4, one S-box kept, `i`/`j` reset per message | `Game::iIgs036_CmdProcessSend` `0x85E35B0`; RC4 `0x85E3020` / `0x85E30B0` |
| Frame checks | game → chip: `+0` low byte = `r` + data sum, `+4` = `r`; chip → game: `+0` = `echo` + data sum, `+4` = `echo`, `+8` low nibble = next offset | `Igs036P2A::SendData` `0x8628AD0`, `ReadData` `0x8628CF0` |
| I/O board record | 20 bytes, byte 19 = sum of 0..18; coin bit 26, service 25, test 24 | `IOBoard::CheckRecvDataChecksum` `0x85F9490` |
| Developer keys (OIS) | key-in bits 0..23 `6 7 8 9 10 12 11 13 73 55 82 181 2 3 4 5 79 80 81 75 76 77 71 72`, 24..26 `20 47 46`; sensors 0..11 `30 33 17 31 32 18 36 39 23 37 38 24` | `0x95328E0`, `0x95328C8`, `0x9532940` |
| Judge lines | sensors per line: 3+4 (surface), 2+5 (rims), 0, 1 | `StagePy::CheckLineHit` `0x8554F50`, table `0x952B0A0` |
| Settings | `rewrite/SettingInfo.dat` (84 bytes): byte 57 coins per credit, 58 free play; network bytes in `PercussionMaster3_Online.md` | `SettingData::SettingData` `0x85B0150` |
| Game link | plaintext: `u16 L` (n + 8, ≤ 4096), `u16` cmd, `u32 n`, data, `u16` CRC-16/ARC (table `0x9541DC0`: `0000 C0C1 C181 0140`, init 0); ping `L` = `0x4BFF`, reply `0x4FFF`, 10 s, drop after 60 s | `MessageSend` `0x8612AF0`, `KoTCP_SendMsg` `0x860C930`, `hash_CRC16` `0x8603960` |
| Challenge reply (0x113, 0x123, `pcli` 0x313) | `sprintf("%08X%08X", a & b, a \| b)` of the server's `u32 a, b`, 16 chars, then CPUID leaf 1 (`eax`, 0, `ecx`, `edx`) and the MAC | `NetManager::NetProcess` `0x85B19F0`; `pcli` `0x805A970` |
| Registration blob (chip 14 / 15 / 16) | `{u32 store id, MAC[6], 0}`, FPGA cipher, key `4e27d199473e7a0f573d82c83743629c` (real mask unknown; the loader's emulated FPGA uses an all-zero mask for this key) | `0x989E1E0`; `NetManager::pubResetNetDataWithEncrypt` `0x85B13C0`, `iRecvNetDataFrom36WithDecrypt` `0x85B1510` |
| Default servers | game `210.5.22.170:9999`, patch `210.5.22.170:9998`, FTP `210.5.22.171:6001`, lists `http://202.153.195.62:8080` | `NetManager::NetInit` `0x85B1DF0`; `pcli` `0x80594E0`; `rewrite/net.lst` |
| Patch FTP login | user `igs_pm3_patch`, password `PM3_PATCH_3293_2011`; `/pm3/patch[_fa]/verNNN/`, `edtNNNMMM/` | `pcli` main `0x805ABC0` |
| Message tags (not sent) | `IGS2011_PercussionMaster3_Game`, `IGS2011_PercussionMaster3_Patch` | `NetManager::NetInit`; `pcli` `0x80594E0` |
| Downloaded lists | first line 32 hex MD5 of the rest (checked, only logged) | `PowerOn` `0x804A31F` |
| Connect test pings | `127.0.0.1`, the FTP host, `168.95.1.1`, the game host | `Setting::ConnectTest` `0x859D630` |
| Sample boards and tickers | kinds `u8[19]` at `0x9540510`, texts `char[19][256]` at `0x9540540` (Big5); contest / dragon texts `0x95402AC`, `0x9540310`, `0x954036C`, `0x95403D8`, `0x9540444`; new-song lists `0x95404D4`, `0x95404E8`, `0x95404FC`; ticker of kind k at `0x9529C80` + 128 × (k + 2) | `BoardInfo::SetFakeData` `0x8602E70`, `RunWordInfo::SetFakeData` `0x85538F0` |
## Asset cipher (sound/*.wav, action/*.rom)

Details: `IGS_SOUND_CRYPTO.md`.

- The 16-byte block shuffle is the same in both games: the `PERM`
  (plaintext byte i from byte `[14,10,8,1,0,12,9,4,3,6,11,7,2,13,5,15][i]`), the
  flipped bytes `{1,4,6,7,10,12,13,15}`, and rotations 1/3.
- Only the final XOR mask differs.  It follows the runtime key the ASIC
  sends; Harpoon stores it at `byte_84059A0`, from message 44.
- `igs_fpga_decrypt.py --game <id>` and the loader's `IGS_ASSET_MASK`
  select the mask; both take it from the game's software-list entry
  (`hash/<machine>.xml`, feature `asset_mask`).

| Game | Mask | How recovered |
| --- | --- | --- |
| Harpoon Lagoon | `eea8543c6a16835d0900d7739f6bf77f` | known WAV/RIFF plaintext, bit statistics |
| Ocean Party | `9528df6122f17c6ec8a249c24b63db9a` | most common block in the WAVs = encrypted silence (zo); mask = zo ⊕ E_harpoon(0) ⊕ MASK_harpoon. Checked on 85/85 WAVs and 26/26 `ACT_ZL3D` banks |
| Hero of Robots (`media/osmbin/*.osmenc`) | `4626ed14dc0ee8baed434037f7574619` | the key arrives in ASIC message 0; mask checked on `BB_Light.osmenc` (decrypts to a boost text archive with a valid length trailer), and the game loads its assets with it in the loader |
| Scratch (`swin`, key `Hello-ScratchWin`) | `e4be1b4e7f081654fa6b81698b039a06` | first block = ELF `e_ident`; the whole file then parses as an i386 ELF. Scratch's assets themselves are not encrypted |

## ASIC handshake and firmware

Details: `IGS_E2100_PROTECTION.md`.

**Session key:**
- The host writes four random words at BAR1 `0x108`.
- The ASIC proves it holds the key `k` with `0xF0F0F0F0 ^ k`; `0x90909090 ^ k`
  means done.
- Both games behave the same here, and the loader's HLE works for both.

**Firmware upload** (only when the ASIC asks with `0xD0D0D0D0`; the HLE never
does):
- **Harpoon:** pages decrypted with the FPGA cipher, key `Open pccard0 !!!`.
- **Ocean Party:** the image is embedded in the binary at `0x8257C00`
  (`0x80000` bytes), with key `Start BigFish6P!` (`sub_8068B80`).
- **Scratch:** embedded at `0x817E900` (`0x80000` bytes), key`
  `pccard0 Open OK!` (`sub_814E620`).  `asic36.bin` is read and ignored.
- **Hero of Robots:** embedded as `A36ROM` (`0x9D45D80`, `0x80000` bytes),
  key `StartHeroOfRobot` (`P2A_LoaderAdv_iLoadDataPointer`).

**Dummy crypto check:** Harpoon runs a 64 KB crypto ioctl with the key
`298yvan42o 89phq24rl;` before reporting done.

## Runtime channel

| | Harpoon Lagoon | Ocean Party |
| --- | --- | --- |
| Cipher | each word XOR key | RC4 stream per direction (KSA with the key as 4 LE bytes; keystream XORed byte-wise over LE words; the stream continues across batches) |
| ASIC → game initial key | `0x82675251` | `0x364251` (`3555921`) |
| game → ASIC initial key | `0x80678267` | `0xA5148F` (`10818703`) |
| Key / index announcements, game → ASIC | type 5, words 11/12 | type 8 (heartbeat, header `392`), word 2 = index (low byte) and key (`>>8`, 24-bit) |
| Key / index announcements, ASIC → game | type 43 (key), type 6 word 12 (index) | type 3, word 11 = index (low byte) and key (`>>8`) |
| Code | `sub_8066450` / `sub_8066590` | `sub_8067130` / `sub_8068910`; RC4 `sub_806AAF0` / `sub_806ABD0` |

**Scratch** has no channel cipher (register-table transport, `Scratch.md`).

**Hero of Robots:** a modified RC4 (`Encrypt::RC4_CypherKey` `0x8918DF0`,
`RC4_CypherData` `0x8918E90`).  There is one S-box (`0x9E7C640`) shared by both
directions, and `i`/`j` restart at 0 on every call.  The game's batch is
encrypted first, then the ASIC's reply.  The game re-keys it:
- **Message 0** (start-up): the key schedule is first run with the 12-byte
  `LF KO IGSA36`; after encrypting, it runs with message 0's payload (8–252
  random bytes).
- **Message 5** (once per attract cycle): after encrypting, the key schedule
  runs with its payload.
- **Message 0 reply** `[flag][32-byte AES key]`: the real key is unknown;
  the loader sends `HEROOFROBOT` zero-padded (its FPGA ignores the key, the
  asset mask above is used instead).
- **Checksum:** trailer dword, low byte = running 8-bit sum of all
  plaintext bytes before it, shared by both directions, starting at 0
  (`Game+0xA20C`).
- **IO board message 4:** 12 bytes, byte 11 = 8-bit sum of bytes 0..10;
  keys = `r1 | r2 << 4 | r3 << 8` (coin `0x2000`, service `0x1000`, test
  `0x800`, up `0x400`, down `0x200`, P1 `0x1/0x2/0x4`, P2 `0x10/0x20/0x40`).
- **Region byte** (message 16 word 2): `0x24` USA (the loader's default),
  0 China, 2 Indonesia, 4 Japan, 6 Korea, `0x0C` Thailand, `0x0F` Hong Kong,
  `0x50` other, anything else Taiwan (`Version::_vVersionInit` `0x88B9E40`).
  The smart card's area byte must match it.

**Hero of Robots smart card** (`HeroOfRobots.md`, Smart card):
- **Applet AID:** `A0 00 00 00 62 03 01 0C 01 01`; INS `0x0F` query, `0x0D`
  debit.
- **Game → card:** RSA-2048, e = `0x010001`, n = the game's built-in
  modulus (`CRSA::CRSA` `0x8CDBFF0`) `C4B6023018F806D1…89D1D625`
  (full value in `games/heroofrobots/card.h`,
  `GAME_KEY_N`).  The private half is on the real card only (unknown).
- **Card → game:** decrypted with the four key parts the ASIC sends in
  message 6 (p, q, p⁻¹ mod q, d).  The real ASIC's key is unknown; **the
  loader's own key** (generated for it, e = 65537, n starting
  `9E07A450503238E5…`, ending `…B5C2886B`) is in `card.h` (`CARD_KEY_*`).

**Hero of Robots QR cards** (`HeroOfRobots.md`, QR cards): the printed
code is decoded by the ASIC (algorithm unknown, firmware not dumped); the
game only sees the card number (message 14).  The loader's NFC tags
(`HeroOfRobots.md`, NFC) hold the 9-digit code in the clear: NDEF external
record `igs-loader:hero`, magic `HERO`, CRC-32 (IEEE, reflected
`0xEDB88320`); no key.

## Board fingerprint (Harpoon Lagoon kernel)

The kernel checks these BIOS strings at boot (table `0xC16EFAA0`).  The
loader runs no kernel, so it never meets this check.

| Address | Expected |
| --- | --- |
| `0xFE0DB` | `09/06/2010` |
| `0xFE0D4` | `V1.10` |
| `0xFE061` | `Phoenix - AwardBIOS v6.00PG` |
| `0xFE049` | `AdvantechIPC` |
| `0xFE0C6` | `DES-BT01` |
| `0xF00F3` | `Atom(` |
| `0xF0067` | `945GS` |
| `0xC0122` | `GT218 Board - 06930002` |
| `0xFEC71` | `09/01/2010-945GS-8A79YAK8C-00` |
