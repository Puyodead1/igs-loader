# libchdr

Unmodified files from libchdr by Romain Tisserand, BSD-3-Clause
(`LICENSE.txt`): MAME's CHD format, read-only.

- Source: https://github.com/rtissera/libchdr, commit
  `47d794151b2b64f6aba60304e6480f4f5a2bc24b` (2026-10-11).
- `src/` (all C sources and their headers; not the optional C++ micro-flac
  backend), `include/libchdr/`, `include/dr_libs/dr_flac.h` (dr_flac 0.13.4
  by David Reid, public domain or MIT-0, see the end of the file).
- `deps/lzma-26.02/`: libchdr's LZMA decoder (LZMA SDK 26.02, public domain,
  `LICENSE`), its symbols renamed `CHDR_*` by libchdr so it links next to
  `third_party/lzma`.
- `deps/zstd-1.5.7/`: zstd's single-file decoder `zstddeclib.c` with
  `zstd.h` and `zstd_errors.h` (BSD, `LICENSE` from
  https://raw.githubusercontent.com/facebook/zstd/v1.5.7/LICENSE).
- Built by `mkromset/sources.sh` with zlib from `third_party/zlib`
  (`CHDR_SYSTEM_ZLIB`) instead of libchdr's bundled miniz.

Used by `mkromset/` to read `.chd` disk images in place; the zstd decoder
also reads zstd SquashFS images.
