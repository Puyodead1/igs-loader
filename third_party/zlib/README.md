# zlib and minizip

Unmodified files from zlib 1.3.1 by Jean-loup Gailly and Mark Adler, under
the zlib licence (`LICENSE`).

- Source: https://zlib.net/fossils/zlib-1.3.1.tar.gz (SHA-256
  `9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23`).
- The library's sources (no gz* files but `gzguts.h`, which `zutil.c`
  includes), and `minizip/` from `contrib/minizip` (Gilles Vollant, zip64
  by Mathias Svensson, same licence): `zip.c`, `ioapi.c` and their headers.
- Used by `mkromset/`: inflate for the filesystems (igs_rofs pages, IGS_SHFS,
  SquashFS), deflate and minizip for `.zip` sets, and libchdr's zlib codec.
