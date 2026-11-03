# LZMA SDK (C subset)

Unmodified files from the LZMA SDK 23.01 by Igor Pavlov, placed in the
**public domain** (see `lzma-sdk.txt`, "LICENSE").

- Source: https://www.7-zip.org/a/lzma2301.7z (SHA-256
  `317dd834d6bbfd95433488b832e823cd3d4d420101436422c03af88507dd1370`),
  directory `C/`.  `Sha1.c`, `Sha1.h` and `Sha1Opt.c` (also public domain)
  are from the 7-Zip 23.01 source, https://www.7-zip.org/a/7z2301-src.7z
  (SHA-256 `fc459c2314a032231c157fd3257906c070ceed11efd6ffa621c2dfebbbc31281`),
  directory `C/`; the SDK leaves them out.
- Used by `romset/` to read `.7z` rom sets: `7zArcIn.c` parses the archive,
  `7zDec.c` decodes its (compressed) header, and `romset/sevenzip.c` streams
  the file data through `LzmaDec`, `Lzma2Dec`, `Bcj2`, `Bra86` and `Delta`
  itself, so memory use is the dictionary size, not the solid block size.
- Used by `mkromset/` (the set builder): `Lzma2Enc`/`LzmaEnc`/`LzFind*`/
  `MtCoder`/`Threads` (multithreaded LZMA2 for `.7z` sets), `Aes`/`AesOpt`
  (the E2100 root filesystem), `Sha1`/`Sha1Opt` (hashes), `Xz*`/`MtDec`/
  `Sha256*` (xz SquashFS images), `Alloc`.

Only the files needed for that are included.
