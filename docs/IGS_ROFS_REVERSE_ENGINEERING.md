# IGS `igs_rofs` / Game Image v2

(E2000's older formats are at the end: [E2000](#e2000-linux-2431-kernels).)

## Scope and preservation boundary

This document describes the read-only Linux root image in
`harpoon-lagoon-p2.img`.  All observations were made from the supplied image
and its bundled kernel.  The original image is never mounted read-write or
modified by the tooling.

## Disk and boot layout

The MBR partition map is:

| Partition | Start LBA | Length | Role |
| --- | ---: | ---: | --- |
| p1 | 63 | 24,740,352 bytes | FAT12 boot partition |
| p2 | 48,384 | 180,633,600 bytes | IGS custom read-only filesystem |
| p4 | 401,184 | 30,449,664 bytes | Linux partition |
| p3 | 460,656 | 1,668,022,272 bytes | SquashFS game assets |

`Fish4P` passes `root=/dev/hda2 ro`.  The expanded `Fish4P-OS` kernel is
32-bit x86 Linux 2.6.32.8 and has `igs_rofs` built in.  Therefore p2 is the
operating-system root, while p3 is the Harpoon Lagoon game-data SquashFS.

## On-disk identification

p2 starts with this ASCII identifier:

```
<International Games System Co., Ltd.> Game Image v2
```

It is not ext2/3/4, FAT, or SquashFS.  The `igs_rofs` mount code validates a
custom superblock and then reads a serialized inode/directory stream.

## Encryption

`igs_rofs` initializes a 128-bit AES key from the literal:

```
Hunter4PFishV1.0
```

The key schedule deliberately transforms the four big-endian source words
before normal AES expansion:

```
w0 = be32(key[ 0: 4]) >> 2
w1 = be32(key[ 4: 8]) >> 1
w2 = be32(key[ 8:12])
w3 = be32(key[12:16]) << 1   (mod 2^32)
```

For this image the effective AES-128 key is:

```
12 1D 5B 9D 32 B9 1A 28 46 69 73 68 AC 62 5C 60
```

AES is ECB/no-padding.  The kernel's decoded superblock output at p2 offset
`0xC8` contains `0xD4AA2684`, the filesystem magic (stored as little-endian
`84 26 AA D4`).  This confirms the key transform against the real image.

## Directory stream (verified)

Directory records are contiguous.  A record comprises `0x68` metadata bytes,
followed by the filename padded to a 16-byte boundary.  A 16-byte encrypted
metadata block at `name_offset - 0x48` supplies the filename length:

```
name_length = (le32(AES_dec(record + 0x20)[4:8]) ^ 0x3A9A44A4) & 0xFF
next_name = name_offset + 0x68 + align16(name_length)
```

The final dword of the metadata is an unencrypted POSIX file mode.  Examples
decoded from the root stream:

| Name | Name offset | Mode |
| --- | ---: | ---: |
| `Fish` | `0x1A8` | `040755` |
| `Fish.bin` | `0x220` | `040755` |
| `bin` | `0x298` | `040755` |
| `var.tar.gz` | `0x9A0` | `0100644` |
| `fish` | `0xA18` | `0100755` |

The stream contains 3,923 records and ends at `0x77038`.  Directories do not
contain a separate B-tree: their decoded inode fields identify a byte range in
this same record stream.  The synthetic mount root is the range
`0x140 + 0x870`; for example, `Fish.bin` has child range
`0x9B0 + 0x78`, which contains its `fish` program entry.  Recursing through
those ranges consumes all 3,923 records exactly once.

## File data and compression (verified)

For an entry at metadata offset `R`, three AES-decoded fields are used by the
kernel:

```
size        = (le32(AES_dec(R + 0x40)[4:8]) ^ 0x3CED596B) & 0x0FFFFFFF
table_offset = ((le32(AES_dec(R + 0x30)[8:12]) ^ 0x9F3EE741) & 0x03FFFFFF) * 4
```

For directories, `table_offset` and `size` are the child-record range above.
For a regular file or symlink, `table_offset` is an array of `ceil(size/4096)`
little-endian physical end offsets.  The first compressed extent begins
immediately after the array; every later extent begins at the preceding array
value.  Each extent is a zlib stream that inflates to one 4 KiB page (the last
page is truncated to the logical file size).

For example, `Fish.bin/fish` has a 2,777,608-byte logical size and a block
table at `0x77038`.  Its table is 679 entries / `0xA9C` bytes, placing the
first zlib page at `0x77AD4`.  This exactly explains the first compressed
region without treating it as a carved file.

## Variant: Ocean Party (`OceanParty-OS`)

Each game's kernel carries its own `igs_rofs` build.  Ocean Party keeps the
overall design (AES-128 with the same key transform, a record stream,
directories as child ranges, one zlib stream per 4 KiB page), but changes
nearly every detail.  Keys and constants are in `IGS_KEYS.md`.

| | Harpoon (`fish4p`) | Ocean Party (`oceanparty`) |
| --- | --- | --- |
| key literal | `Hunter4PFishV1.0` | `PartyOcean6PV1.0` |
| field XOR delta from init constants | `−0x3CE8` | `∓0x805E` |
| superblock read | magic at block `0xC8`, word 1 | 336-byte superblock from offset 0; magic at block `0x58`, word 0, plus four XOR consistency checks |
| first record | `0x140` | `0x150` |
| record header | `0x68` bytes, mode at `+0x64` | `0x78` bytes, mode at `+0x74` |
| name length | `dec(R+0x20)[1]` | `dec(R+0x50)[3]`, low byte |
| size | `dec(R+0x40)[1]` | `dec(R+0x30)[1]` |
| table offset (×4) | `dec(R+0x30)[2]` | `dec(R+0x00)[3]` |
| per-record check | — | `dec(R+0x10)[1] ^ C1 = 0xA5A5A5A5`, `dec(R+0x20)[3] ^ C2 = 0xAA55EB48` |
| names | plaintext, padded to 16 | AES-CBC, same key, zero IV per name, padded to 16 |
| page table | 4-byte LE end offsets | one encrypted 16-byte block per page; end offset = word 0 |
| zlib pages | stored as is | the 2-byte zlib header is zeroed on disk; the kernel writes `78 9C` back before inflating (`sub_C102016C`) |
| records | 3,923 | 3,946 |

Kernel functions in Ocean Party's build:

| VA | Role |
| --- | --- |
| `0xC101F188` | metadata decrypt (AES-ECB, key copy at `0xC192E4C0`) |
| `0xC101F0F0` | name decrypt (AES-CBC, key copy at `0xC192E4D0`, IV `0xC192E4F8` zeroed per call) |
| `0xC101F2A8` | table offset and size of a record |
| `0xC101F370` | inode from a record (checks, mode, size) |
| `0xC101F792` | mount / superblock checks |
| `0xC101FB64` | readdir |
| `0xC101FD83` | lookup |
| `0xC101FFE5` | readpage |
| `0xC102016C` | zlib header restore + inflate |
| `0xC101F1B9` | per-file CRC-32 check |

## Variants: Scratch and Hero of Robots

Both were reversed from their kernels (`Scratch-OS`, `HeroOfRobots-OS`);
the constants are in `IGS_KEYS.md`.  Each verifier walk consumed every
record exactly once and inflated every file.

| | Scratch (`scratch`) | Hero of Robots (`heroofrobots`) |
| --- | --- | --- |
| key literal | `SwinV1.00Scratch` | `RobotsHeroOfV1.0` |
| design | Ocean Party's (encrypted names, encrypted page table, blanked zlib headers) | Harpoon's (plain names, plain page table, stock zlib) |
| XOR delta | `±0xA631` | `±0x265B` |
| magic | block `0x74` (not 16-aligned), word 2 | block `0xF0`, word 3 |
| first record / header | `0x150` / `0x78` | `0x140` / `0x68` |
| mode | `+0x72` (uid `+0x70`, gid `+0x74`) | `+0x64` (gid `+0x60`, uid `+0x61`) |
| name length | `dec(R+0x00)[3]` | `dec(R+0x40)[2]` |
| size | `dec(R+0x30)[1]` | `dec(R+0x30)[0]` |
| table (×4) | `dec(R+0x50)[1]` | `dec(R+0x00)[2]` |
| page table | one encrypted block per page, end offset = word 1 | 4-byte LE end offsets |
| extras | per-file CRC-32C from `dec(R+0x10)` (not checked by the extractor) | names padded with junk bytes; the root record uses a different table XOR |
| records | 3,858 | 3,963 |

In both, the root record is a full record image in the superblock (Scratch
`+0x94`, Hero `+0x40`).  Its range ends where the first child range starts,
so the extractor's root rule works unchanged.

**Scratch's executable is encrypted a second time.**  `/Scratch/swin` is
FPGA-cipher data that `/bin/unrar` (an IGS tool, not RAR) decrypts at boot
with the key string `Hello-ScratchWin` (`IGS_KEYS.md`).  The rom-set builder
does the same; the plain ELF is the set's `/Scratch.bin/swin`.

Root directory: as in Harpoon, the mount record lives in the superblock.  Its
children run from the first record to the start of the first child range
(`0x150`–`0xAE0` here, `0x140`–`0x9B0` in Harpoon).

## Extractor

`romtool\IgsRofsExtractor` is a standalone .NET 8 implementation of the rules
above.  It uses no binwalk output or signature carving.  The format profile
is detected by which key decodes the superblock magic; `--profile` forces
one.  Run it from the game's folder:

```powershell
dotnet run --project .\romtool\IgsRofsExtractor\IgsRofsExtractor.csproj -- `
  .\p2.img --extract .\output
```

A new game whose kernel uses another key or layout needs a new entry in
`RofsProfile.All`.  The key literal sits next to the `igs_rofs` init code
(the function that stores the five constants).  Field positions come from
the mount, inode and readpage functions listed above.

Use `--list` to print decoded metadata instead.  The extractor rebuilds the
directory tree, inflates regular-file and symlink contents from their mapped
zlib blocks, creates symlinks where the host permits it, and records device
nodes it cannot represent on Windows in `IGS_ROFS_SPECIAL_NODES.txt`.

`romtool/igs_rofs.py` is a Python port with the same
profiles and the same CLI (`igs_rofs.py <p2.img> [--list] [--extract <dir>]
[--profile <name>]`).  It keeps file modes, skips special nodes instead of
listing them, and is what the rom-set builder (`romtool/mkromset.py`,
`IGS_LOADER.md`, Rom sets) uses.  Its output matches the C# tool's file for
file (checked on Scratch's p2).  A new profile goes into both.

## Kernel evidence

| Address | Role |
| --- | --- |
| `0xC101F0F0` | AES metadata decrypt wrapper |
| `0xC101F121` | Decodes an inode identifier from metadata |
| `0xC101F631` | `igs_rofs` mount/superblock parser |
| `0xC101F9F1` | Directory iteration |
| `0xC101F1EF` | Builds an inode from a decoded metadata record |
| `0xC101FE00` | Reads and inflates a mapped 4 KiB file page |
| `0xC1C64926` | `igs_rofs` initialization, including AES key setup |

The driver requires 4 KiB compressed blocks and contains zlib inflate support.
Carving individual gzip/zlib signatures is not a valid extraction method: it
does not recover the directory tree or the file-to-block mapping.

## Validation

The extractor completed all 3,923 records from this p2 image and rebuilt the
tree at `harpoon-lagoon-p2-extracted-v2`.  The only Windows-unrepresentable
entries are `/dev/console` (mode `02180`) and `/dev/null` (mode `021B6`),
listed in its special-node manifest.  Its extracted `Fish.bin/fish` is
identified as a 32-bit i386 ELF executable with the expected dynamic loader
`/lib/ld-linux.so.2`; `etc/fstab` expands as readable Linux configuration.

This verifies directory ownership, metadata decryption, compressed extent
mapping, and zlib decoding against actual application and configuration files.

# E2000 (Linux 2.4.31 kernels)

E2000 images predate "Game Image v2".  Nothing is AES-encrypted: the root
filesystem is a cramfs variant, the game data is a SquashFS 2.x variant, and
the executable is split into pieces that a two-stage boot loader reassembles.
Reversed from Gold Captain (`GoldCaptain-OS-e65`, `2.4.31-IGS_V1.0`, built
1 Dec 2008).  One command does all of it:
`romtool/igs_e2000_extract.py <id> <image> <work dir>`, with
the constants from the software list (`hash/e2000.xml`); the loader's
rom sets come from `romtool/mkromset.py`, which reuses its assembly.

## Disk layout

| Partition | Content | Mounted at |
| --- | --- | --- |
| p1 ext2 | LILO map (`<Game>`), bzImage `<Game>-OS-eNN`; a raw stage-2 stub at a fixed sector | – |
| p2 | plaintext ROFS (below), the root filesystem | `/` (`root=/dev/hdc2 ro`) |
| p3 | IGS_SHFS (below), game data | `/<Game>` (mounted by the kernel itself) |
| p4 ext3 | settings / bookkeeping | `/<Game>/var` (mounted by the kernel itself) |

`init/main.c` is patched.  Before `/sbin/init` it:
1. mounts `shfs /dev/hdc3 /<Game>` and `ext3 /dev/hdc4 /<Game>/var`;
2. mounts tmpfs on `/mnt` and runs
   `/bin/csh /lib/libdb0.a __KERNEL__ "-no-print -GC"` (`0xC0105054`);
3. runs `/mnt/GECA` with the same arguments, then unmounts `/mnt`;
4. hangs unless every stage set its bit in `0xC0709090`.

The kernel strings say "IGS-j filesystem" for this game-data scheme.

**Pirate Revenge** (`PirateRevenge-OS-57P`, `2.4.31-IGS_V0.5a`, built 2 Aug
2007) differs:
- p3 mounts on `/pr/linux`, p4 (ext2, `gameset.bin`) on `/fec_data`.
- Stage 1 is `/bin/HWtest`, run as `/etc/rc.d/rc0.d __KERNEL__ "-no-print -PR"`.
- `/etc/X11/IGS` runs `cd /pr/linux; ./sship`, the copy on p3 (unstripped).
  The assembled `/pr/exec/sship` (a ramfs over a static decoy of the same
  name) has the same code and is stripped.

## Plaintext ROFS (p2)

The format is cramfs with an IGS superblock; there is no encryption.  Kernel
code: superblock `0xC0484974`, inode fill `0xC04845D0`, block reader
`0xC048472C`.

- **Superblock:**
  - `+0x00`: a word with `w ^ magic == 0x55AA48EB` ("minor").
  - `+0x04`: the text `<International Games System Co., Ltd.> Game Image `.
  - `+0x4C`: magic `0xD4AA2681`.
  - `+0xA8`: the root inode.
- **Game type check:** `sb[0x04] ^ root[0] == 0xAD2423DB`, where `root[0]`
  is the root inode's version word (`0xDB5A3430`).
- **Inode version check** (every iget): `inode[0] ^ root[0] == 0xEC3BC248`.
  Every non-root inode in Gold Captain stores `0x3761F678`.
- These are the same two constants that the IGS_SHFS superblock stores at
  `0x48` / `0x4C`.
- **Inode, 16 bytes:**

  | Offset | Field |
  | --- | --- |
  | `+0` | version word; every inode stores the key, except the root |
  | `+4` | `namelen:6 \| offset:26`, both in 4-byte units |
  | `+8` | `size:24 \| unused:8` |
  | `+12` | `mode:16 \| uid:8 \| gid:8` |
  | `+16` | the name, padded to 4 bytes |

- **Directories:** the entries are packed at `offset`; `size` is their
  total length in bytes.
- **Files and symlinks:** a table of `ceil(size/4096)` block end pointers,
  then one zlib stream per 4 KiB block (cramfs).
- **Tool:** `romtool/igs_rofs24.py`.

## IGS_SHFS (p3)

This is SquashFS 2.x (64 KiB blocks, zlib) behind an IGS superblock.  The
kernel calls it `shfs`, with errors `IGS_SHFS error: ...`; the `read_super`
is around `0xC0488400`.  Everything after the superblock is stock SquashFS
2.x: metadata blocks, 2.x inodes (types 1–8, including the long directory),
directory headers, data blocks with `1<<24` meaning uncompressed, and the
fragment table.

The superblock is packed and little-endian:

| Offset | Field | Gold Captain |
| --- | --- | --- |
| `0x00` | magic `0xD4AA2682` (`0xD4AA2683` = other endian) | |
| `0x04` | `<International Games System Co., Ltd.> Game Image ` | |
| `0x48` | game type, compared with a kernel constant | `0xAD2423DB` |
| `0x4C` | game version | `0xEC3BC248` |
| `0x50` | major (obfuscated) | `0xDE5550B3` |
| `0x54` | minor (obfuscated) | `0x8448B33B` |
| `0x58` | inodes | 1232 |
| `0x5C` | bytes_used | |
| `0x60` | uid_start | |
| `0x64` | guid_start | |
| `0x68` | inode_table_start | |
| `0x6C` | directory_table_start | |
| `0x70` | flags | `0x40` |
| `0x71` | no_uids | |
| `0x72` | no_guids | |
| `0x73` | mkfs_time | |
| `0x77` | root_inode (u64: `block<<16 \| offset`) | |
| `0x7F` | block_size | 65536 |
| `0x83` | fragments | |
| `0x87` | fragment_table_start | |

**Plain layout.**  Pirate Revenge and Speed Driver 2 use a shorter form:
- There is no text and no game type/version.
- The magic is followed by `0x00100000`, then major `2` and minor `1` in
  plain at `+0x08` / `+0x0C`.
- From the major word on, the fields are at the same relative offsets as
  above (major at `M` = `0x50` or `0x08`; inodes at `M+8` … fragment table
  at `M+0x37`).
- `igs_shfs.py` detects the layout by the text.

| Image | Layout | Game type | Root listing |
| --- | --- | --- | --- |
| Go Go Ball 2 DX | Game Image | `0x5CAAEC43` | `linux pitch system sound main` |
| Gold Captain | Game Image | `0xAD2423DB` | `demo flc image keyindat main net …` |
| Panda Family (CN) | Game Image | `0x2AD566DC` | `PF Reset27 demoplay flc main pf_data …` |
| Pirate Revenge m101cn | plain | – | `keyindat sship image sound …` |
| Speed Driver 2 | plain | – | `data io.inf sd2` |
| Speed Rider m109cnc | Game Image | `0x3AEF3EEC` | `data bin_linux` |
| We Dance Online 2 | Game Image | `0x8E1673C2` | `CFVER net net.cfg wedoCliPatch.exe …` |

All seven E2000 images use the plaintext ROFS (`0xD4AA2681`) on p2.

Notes for implementers:
- A directory's `file_size` is 3 more than its listing length.
- `romtool/igs_shfs.py` extracts it, or lists it with `--list`.  Gold Captain:
  46 directories, 1185 files, 1.06 GB.

## Executable assembly (stage 1 `csh`, stage 2 `GECA`)

The game executable is never stored whole.  The root filesystem's
`/<Game>/<Game>` is a symlink to `/Game/<Game>`, a ramfs that the loaders
fill at boot.

**Stage 1:** `/bin/csh` (a static, stripped ELF; `sub_80481D0`):
- `/mnt/head` = 1024 bytes at p2 sector `HEAD_SECTOR`.  These are the
  executable's first KiB.
- `/mnt/GECA` = 1024 bytes at p1 sector `GECA_SECTOR`, followed by
  `/lib/libdb0.a`.  The result is an ELF.
- Without these arguments it prints a fake H/W test.  `--james` prints the
  product string (`GoldCaptian-E2000-v1-e65`).
- It refuses to run unless `/dev/pccard0` opens and a hardware check passes.

**Stage 2:** `GECA` (`sub_804893C`; constants at `0x804B54C`):
- It mounts `ramfs GameExecution` on `/Game`.
- It writes `cat /mnt/head /mnt/libdb<d> ... > /Game/<Game>`, where each
  piece is `dd` of `/lib/libdb<d>.a`:
  - Pieces are taken in the order of the digit string `PIECES`.
  - Each piece keeps its first `size/1K - trim` KiB.
  - `trim` starts at `TRIM_KB` and changes by `TRIM_STEP` per piece.
  - The last piece keeps `size - trim*1024` bytes.

**Pirate Revenge** (`/bin/HWtest` `sub_80481D0`, GECA `sub_80488C8`,
constants at `0x804B34C`) swaps the partitions and keeps the other end of
each piece:
- `/mnt/head` = p1 sector 6813; `/mnt/GECA` = p2 sector 125515 +
  `/etc/rc.d/rc0.d`.
- The ramfs is `/pr/exec`, the executable `sship`; the pieces are
  `/etc/rc.d/rc<d>.d`.
- Each piece is `dd ... bs=1K skip=n`: everything after its first `n` KiB,
  `n` starting at 9 and growing by 4.

Speed Driver 2 and Speed Rider use a third mode, `cut` (Speed Rider GECA
`sub_8048958`): each piece is `dd bs=1K skip=s count=size/1K-s-t`, the
last one `bs=1 skip=s*1024 count=size-(s+t)*1024`; `s` and `t` each have
their own step.  Empty or missing pieces are passed over without stepping.

`igs_e2000_extract.py` checks the result: the ELF section-header table must
end exactly at the end of the file.  Software-list features: `head_part`,
`geca_part`, `geca_head` (a root-fs file whose first KiB is the stub, Panda
Family), `geca_file`, `piece_file`, `piece_mode` (`trim` / `skip` /
`cut`), `trim_kb` / `trim_step` for `n` (`s` in `cut`), and `cut_kb` /
`cut_step` for `t`.

| Game | Stage 1 | GECA | Head | Pieces | Mode | n, step | Executable | Size |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Gold Captain | `/bin/csh` | p1 6813 + `libdb0.a` | p2 125515 | `/lib/libdb<d>.a`, `47632518` | trim | 1004, −17 | `/Game/GoldCaptain` | 1,506,192 |
| Pirate Revenge | `/bin/HWtest` | p2 125515 + `rc0.d` | p1 6813 | `/etc/rc.d/rc<d>.d`, `6415237` | skip | 9, +4 | `/pr/exec/sship` | 2,336,328 |
| Go Go Ball 2 DX | `/bin/csh` | p1 6692 + `rc0.d` | p2 129912 | `/etc/rc.d/rc<d>.d`, `2163547` | skip | 168, −17 | `/GoGoBall2/linux/pitch` | 1,936,100 |
| Panda Family (CN) | `/bin/csh` | first KiB of `/bin/tcsh` + `/bin/ksh` | p1 6658 | `/lib/libfun<d>.so`, `413256` | skip | 2, +12 | `/linux/PF` | 2,100,388 |
| Speed Driver 2 | `/bin/HWtest` | p2 114921 + `rc4.d` | p1 7145 | `/lib/libc<d>.so`, `143287569` | cut | s 3 +4, t 141 −9 | `/race/exec/sd2` | 4,134,548 |
| Speed Rider m109cnc | `/bin/csh` | p1 7145 + `rc0.d` | p2 114921 | `/etc/rc.d/rc<d>.d`, `147568329` | cut | s 17 +3, t 413 −9 | `/sr/exec/SR` | 11,010,392 |
| We Dance Online 2 | `/bin/tcsh` | p1 6693 + `rc0.d` | p2 129918 | `/etc/rc.d/rc<d>.d`, `314256` | skip | 299, −3 | `/WeDo/exec/poweron` | 43,756 |

Sectors are relative to the partition.  Each stage 1 runs as `<GECA
file> __KERNEL__ "-no-print -<tag>"` (tags `GC`, `PR`, `GGB2`, `PF`, `SD2`,
`SR`, `WeDo`).  The static ELFs at the executables' paths on p2/p3 (Go Go
Ball 2's `pitch`, Panda Family's `/linux/PF`) are decoys that wipe the disks.

| Game | p3 (IGS_SHFS) at | p4 (ext3) at |
| --- | --- | --- |
| Go Go Ball 2 DX | `/GoGoBall2` | `/rf_data` |
| Panda Family (CN) | `/PF` | `/PF/pf_data` |
| Speed Driver 2 | `/race/linux` | `/data` |
| Speed Rider m109cnc | `/sr/Game` | `/rewrite` |
| We Dance Online 2 | `/WeDo/system` | `/WeDo/linux` (3.6 GB: the game data and `wedo`) |

**We Dance Online 2** assembles only its launcher, `poweron`; the game,
`wedo` on p4, is packed with stock UPX 3 (linux/elf386, NRV2E).  After the
stub's ELF and program headers: `l_info` {checksum, `UPX!`, size, version
13, format 12}, `p_info` {0, file size, block size}, then blocks of
`b_info` {uncompressed, compressed, method, filter, cto, 0} and their data
until an uncompressed size of 0.  The first block is the original ELF and
program headers, then each `PT_LOAD`'s file bytes, then the gaps between
and after them.  `romtool/igs_upx.py` unpacks it (NRV2B/D/E); the rom set
keeps `wedo` as dumped and adds the unpacked copy at `/WeDo/exec/wedo`
(build step `unupx`).  `WeDanceOnline2.md` has the launcher and the boot
scripts.

# E1000 (Linux 2.4.24-gx)

Percussion Kids M105CN: no IGS formats.  Five plain ext2 partitions; p4 is
an extended partition holding p5.  GRUB boots `bzImage-2.4.24-GX_M-CM_K-PCI_K`
from p1 with p1 as the root; BusyBox `init` runs `/etc/init.d/rc.sysinit`:

| Partition | Content | Mounted at |
| --- | --- | --- |
| p1 | root: BusyBox, uClibc 0.9.26, SDL, SDL_mixer | `/` |
| p2 | `system/gx2show`, `load.bmp` (the boot picture) | `/pkids/gamedata` |
| p3 | the game: `pkids`, `main/`, `band1/`, `sound/`, `system/` | `/pkids/linux` (read-only) |
| p5 | `assert.txt` (the game's error log) | `/pkids/log` |

`rc.sysinit` shows `load.bmp` with `gx2show`, then `/etc/init.d/pkids_game`
runs `cd /pkids/linux; ./pkids`.  `romtool/mkromset.py` reads logical
partitions as p5 and up.
# E2200 (Linux 2.6.32.8)

Percussion Master 3 M107FA.  The root filesystem is a Game Image v2
(`igs_rofs`) with Ocean Party's design: profile `pmaster3`, key literal
`E2200-PMaster-V3`, constants in `IGS_KEYS.md`; the extractors' walk
consumed all 3,904 records and inflated every file.  LILO boots
`PM3-OS` from p1 with p2 as the root.

| Partition | Content | Mounted at |
| --- | --- | --- |
| p1 ext3 | LILO map `PM3`, `boot.b`, kernel `PM3-OS` | – |
| p2 | `igs_rofs`: the root (Fedora-based, X.org, NVIDIA GL), `/PM3/PowerOn` (encrypted), `/bin/uudecode`, `/bin/mountloop` | `/` |
| p3 ext3 | `ROMS/*.rom`: 32 SquashFS 4 (zlib) images; empty `exe`, `data`, `media` | `/PM3` (by `/etc/X11/IGS`) |
| p4 ext3 | settings, bookkeeping, rankings, downloads | `/PM3/rewrite` |

`/bin/mountloop` loop-mounts `exe.rom` on `/PM3/exe`, `data.rom` on
`/PM3/data` and every other `<name>.rom` on `/PM3/media/<name>`.  The
launcher and the executable are FPGA-cipher encrypted (`IGS_E2200.md`);
`romtool/mkromset.py`'s `loop` and `poweron` steps do the rest.