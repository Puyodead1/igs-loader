# LZMA SDK (C decoder subset)

Unmodified files from the LZMA SDK 23.01 by Igor Pavlov, placed in the
**public domain** (see `lzma-sdk.txt`, "LICENSE").

- Source: https://www.7-zip.org/a/lzma2301.7z (SHA-256
  `317dd834d6bbfd95433488b832e823cd3d4d420101436422c03af88507dd1370`),
  directory `C/`.
- Used by `romset/` to read `.7z` rom sets: `7zArcIn.c` parses the archive,
  `7zDec.c` decodes its (compressed) header, and `romset/sevenzip.c` streams
  the file data through `LzmaDec`, `Lzma2Dec`, `Bcj2`, `Bra86` and `Delta`
  itself, so memory use is the dictionary size, not the solid block size.

Only the files needed for that are included.
