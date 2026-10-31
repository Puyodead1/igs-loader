# IGS "FPGA_AES" asset encryption (sound/*.wav, action/*.rom)

## Where it happens

`fish` loads assets through `sub_8068320` (sounds: `sub_806B610` ->
`sub_8064C50` -> `SDL_RWFromMem` / `Mix_LoadWAV_RW`).  The whole file, padded
to 16 bytes, is passed to `sub_80A2998` ("FPGA_AES_iEncrypt"), which sends it in
64 KiB chunks to `/dev/pccard0` with ioctl `0xC0106610`.  The FPGA does the work.

The 16-byte key is not on disk.  `byte_84059A0` is initialized to `FF` and is
filled by protection-message type 44 (`sub_8066730`), then copied in by
`sub_80682A0` / `sub_80689E0` at startup.  This is the FPGA mailbox path;
the loader's E2100 machine applies the cipher itself.

## The cipher is not AES

The cipher works on 16-byte ECB blocks, and every position uses the same transform
(silence blocks repeat across files and offsets).  It is a fixed bit shuffle
with no diffusion:

```
u        = plaintext bytes placed by PERM (u[PERM[i]] = p[i])
u[j]    ^= 0x80          for j in {1,4,6,7,10,12,13,15}
lo, hi   = le64(u[0:8]), le64(u[8:16])
c        = rotl64(lo, 1) || rotl64(hi, 3)   XOR  MASK

PERM = [14,10,8,1,0,12,9,4,3,6,11,7,2,13,5,15]
MASK = ee a8 54 3c 6a 16 83 5d 09 00 d7 73 9f 6b f7 7f
```

These values are for Harpoon Lagoon's runtime key.  Ocean Party shows that
only `MASK` changes between games: same `PERM`, flips and rotations, with
mask `9528df6122f17c6ec8a249c24b63db9a`.

To recover a new game's mask:

1. Take the most common 16-byte block across its WAVs.  That is encrypted
   silence, `zo`.
2. Compute `MASK = zo ^ E_harpoon(0) ^ MASK_harpoon`.  The flips are
   constant XORs, so they fold into the mask.
3. Check that the RIFF/fmt headers come out right.

Per-game masks are in `IGS_KEYS.md`.

## How it was recovered

- The XOR differences of block 0 across 123 WAVs have GF(2) rank 22, which matches
  the number of varying RIFF-size bits.  Size bits map 1:1 to cipher bits
  (size bits 16-22 -> cipher bits 75-81, bits 9-15 -> 100-106).
- Bit correlations across music data show 16 runs of 8 consecutive bits:
  8 strongly correlated runs (PCM high bytes) and 8 weakly correlated runs
  (low bytes).  The runs start at bit 1 mod 8 in the low half and bit 3 mod 8
  in the high half, which gives the two rotations.
- `MASK` is the ciphertext of an all-zero block.  The byte order and bit-7
  inversions come from the known `RIFF....WAVEfmt ` and `fmt ` plaintext.

## Tool and validation

`romtool/igs_fpga_decrypt.py <in dir> <out dir>` decrypts a folder of
assets, for example the game partition's `sound/` and `action/`.  `--game`
picks the mask from the software list; the default is Harpoon Lagoon's:

```
python3 romtool/igs_fpga_decrypt.py <set>/Fish/sound  decrypted/sound
python3 romtool/igs_fpga_decrypt.py --game oceanparty_md105cn <set>/OceanParty/action decrypted/action
```

All 123 decrypted WAVs parse with consistent RIFF/fmt/data sizes and smooth
PCM.  The `.rom` files decode to `ACT_ZL3D` containers that hold DDS/TGA
textures.  Outputs keep the 16-byte padding (the RIFF size excludes it).
