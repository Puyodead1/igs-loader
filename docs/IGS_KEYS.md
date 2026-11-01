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

**Other E2000 images** (triage only; executables assembled, not run):

| Game | Stage 1 / tag | GECA | Head | Pieces | Executable |
| --- | --- | --- | --- | --- | --- |
| Panda Family (CN) `PF-E2000-v1-e65` | `/bin/csh`, `-PF` | first KiB of `/bin/tcsh` + `/bin/ksh` | p1 6658 | `/lib/libfun<d>.so`, `413256`, skip 2 +12 | `/linux/PF` |
| Speed Driver 2 `SD2-E2000-v05-e61` | `/bin/HWtest`, `-SD2` | p2 114921 + `rc4.d` | p1 7145 | `/lib/libc<d>.so`, `143287569`, cut s 3 +4, t 141 −9 | `/race/exec/sd2` |
| We Dance Online 2 `WeDo-e2000-v1-e64` | `/bin/tcsh`, `-WeDo` | p1 6693 + `rc0.d` | p2 129918 | `/etc/rc.d/rc<d>.d`, `314256`, skip 299 −3 | `/WeDo/exec/poweron` |

Constants: Panda GECA `0x804B5CC`, Speed Driver 2 `0x804B54C`, We Dance
`0x804B5EC`.  IGS_SHFS game types: Panda `0x2AD566DC`, We Dance
`0x8E1673C2`; Speed Driver 2 has the plain layout.

## Asset cipher (sound/*.wav, action/*.rom)

Details: `IGS_SOUND_CRYPTO.md`.

- The 16-byte block shuffle is the same in both games: the `PERM`, the
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
