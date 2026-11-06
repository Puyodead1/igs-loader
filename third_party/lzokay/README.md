# lzokay (LZO1X decoder)

lzokay by Jack Andersen, **MIT licence** (`LICENSE`).

- Source: https://github.com/AxioDL/lzokay, commit
  `db2df1fcbebc2ed06c10f727f72567d40f06a2be` (archive
  https://codeload.github.com/AxioDL/lzokay/zip/db2df1fcbebc2ed06c10f727f72567d40f06a2be,
  SHA-256 `bb4937ad2baf81d48940af8fcf9e2d15eb648e5502e70d0806a4b95561751fa8`).
- `lzokay-c/lzokay-c.h`, `lzokay-c/lzokay-c.cpp`, `LICENSE`: unmodified.
- `lzokay.cpp`: its first 292 lines (the decoder) and the namespace's closing
  brace; the encoder after them is left out.  `lzokay.hpp`: the result codes
  and `decompress` only (the encoder's dictionary types and `compress` left
  out).
- Used by `mkromset/` (the set builder) for LZO-compressed SquashFS 4 images
  (Crazy Shoot's p3), through the C wrapper `lzokay_decompress`.
